# Document simulation session boundary

`DocumentSimulationSession.*` introduces the standard C++20 application layer in
`src/cpp/application`, built as `hps_application`. It owns a background simulation
service, tags results with document/session identity and projects value data for
presentation. It has no MFC, window pointer, frontend callback or write-back path.
The existing MFC document does not own or activate it yet.

## Identity and ownership

The caller supplies a nonzero typed 64-bit `DocumentId` for the open document.
Successful creation assigns a distinct typed `SessionId` from a checked atomic
counter. IDs are runtime identities within the linked application, not archive
identities, paths or content hashes. Exhaustion throws before reuse; reopening or
re-exporting a document creates a new session ID even with the same document ID.

`SessionIdentity` combines both IDs. `SessionSnapshot` and `SessionCommand` carry
that identity alongside an immutable publication or command future. A command's
service reply is interpreted together with its enclosing tagged handle. The UI
must compare the handle's identity with its currently active session before using
results. `Accepts(identity)` requires an exact match and an open session. It
rejects old-session results without inspecting freed document addresses.

Creation returns an owned `unique_ptr` or structured validation issues. Identity,
batch/queue options, model/topology, timing and legacy ranges are validated before
starting the worker. The session owns copied engine data, topology and clock;
neither service nor application layer retains the source document. The private
constructor is immediately wrapped in `unique_ptr` for RAII ownership.

## Owner thread, presentation and closure

All session methods require the creating thread and throw `logic_error` before
accessing state on other threads. Command futures, completion futures and retained
immutable snapshots can outlive the session. Destruction joins the worker through
the existing service owner. Callers must finish their own session method calls
before destroying it.

`Presentation()` returns an independent value containing identity, publication
revision, run state, marking/statistics, transition timing/activity, clock/stop
reasons, diagnostics and closure state. It omits engine RNG and graph definitions
from the presentation projection. `Snapshot()` retains the full immutable engine
publication for application/replay needs. Neither API updates MFC objects.

`Close()` invalidates acceptance before requesting service shutdown/join. It is
idempotent on normal completion and rethrows unexpected worker errors through the
service. New commands on a closed session throw `logic_error`; historical tagged
handles remain readable but are no longer accepted. Save `Completion()` or use
explicit Close to observe unexpected worker failure; destruction only joins.

`WaitForUpdate` is an owner-thread bounded-wait convenience for tests/noninteractive
callers. A future UI adapter should poll snapshots/futures without blocking window
event processing. No worker-to-UI callback or synchronous message is introduced.

## Offline MFC adapter

`CreateLegacyDocumentSession` in `adapters/LegacySessionAdapter.*` must run on the
document-owning thread with legacy simulation offline. It uses the validated net/
topology export, preserves current marking/delays/flags, and copies exact logical
time, step count, sample interval and time/step limits. Three unused read-only
getters in `HSimulation.h` expose the latter settings. No default clock substitution,
implicit reset, document mutation or legacy worker start occurs.

Online documents return `SnapshotUnavailable`. Malformed exports and unsupported
settings propagate diagnostics before creating a service. The current API cannot
independently discover a legacy document's owning thread; its export precondition
remains the caller's responsibility. Session method affinity is enforced afterward.

Source edits after export do not change session data. Source destruction is safe
for value ownership, but **does not automatically close the session**: MFC lifecycle
hooks are not installed. Integration must call Close on document close/replacement
and invalidate/recreate the session on relevant edits before accepting results.
Identity checks alone do not detect edits within the same still-open session.

## Verification (2026-10-28)

Ten portable cases cover tagging, replacement/stale replies, close/rejection,
input/projection isolation, seeded presentation, wrong-thread calls, diagnostics,
validation, destruction/retained handles and concurrent unique-ID creation.

Five legacy cases cover exact current clock/timing export, source edits without
write-back, source document destruction before running the value-owned session,
validation errors, and creating/closing sessions from all six read-only sample
archives. Existing online-guard coverage now includes session creation without
starting a legacy worker. No model conversion or save occurs.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, libraries and test build | Passed | Passed | Passed | Passed |
| Previous cases (online guard extended) | 237/237 | 237/237 | 237/237 | 237/237 |
| Portable session cases | 10/10 | 10/10 | 10/10 | 10/10 |
| Legacy session cases | 5/5 | 5/5 | 5/5 | 5/5 |
| CTest suites | 28/28 | 28/28 | 28/28 | 28/28 |

Win32 uses `build/vs2022-win32`; x64 uses `build/rules-x64`. Domain, simulation and
application libraries also build without MFC in `build/rules-domain-only` Release.
New application/test targets use strict MSVC warnings; existing legacy warnings
remain visible. GUI flows, actual MFC window closure, printing, animation and
worker replacement remain integration work. Serialization and samples are unchanged.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^application\.' --output-on-failure
```

## Next boundary and rollback

Add a controlled binding to document-edit/replacement/close events, enforcing
invalidation and service closure on the UI owner thread before rendering results.
Keep it separate from replacing simulation commands or applying markings to
legacy objects. Test multi-document switching and delayed replies before activation.

Rollback removes `src/cpp/application`, `LegacySessionAdapter.*`, the three legacy
getters, application/test CMake entries, and new session tests/online assertions.
Restore the adapter target's prior source/link list. Prior domain/simulation/net
export APIs remain usable, with no archive or user data migration.
