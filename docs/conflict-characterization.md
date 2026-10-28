# Legacy conflict selection: characterization before extraction

Eight new cases in `tests/SimulationCharacterization.cpp`, tagged
`[conflict-characterization]`, execute the real synchronous `SingleStep` conflict
pass. They extend the existing branch/meet and inactive-first-arc cases. No
production implementation, domain API, archive format or sample file changes in
this stage.

## Why selection needs another input boundary

`PetriNet::arcs` currently preserves document object order. The real conflict pass
uses each node's `ConnectorsIn()` and `ConnectorsOut()` array order. These orders
can differ: `CHConnector::From/To` removes a connector from an old endpoint and
appends it to a new endpoint through `CHNetMember::AddOut/AddIn`. Reconnecting to
the same endpoint after detaching changes adjacency order without changing the
document list.

The new reconnection case proves this for both branch and meet conflicts. The
adapter still exports `[firstArc, secondArc]`, while the place's adjacency array
contains `[secondArc, firstArc]`. With seed 1, the second transition wins.
Consequently a pure selector that reconstructs adjacency solely by filtering
`snapshot.net.arcs` would silently change current behavior for these documents.
This is a limit of the current export for conflict replay, not a failure of its
existing eligibility, token-effect or timing use cases.

## Algorithm observed in the source

`CHSimulation::Online()` scans transitions in document order and records potential
conflicts when an input place has multiple outgoing connectors or an output place
has multiple incoming connectors. Arc kind and available resources do not filter
this pre-scan. The fixture supplies that list explicitly and does not execute
`Online()` or start its worker; pre-scan equivalence is still untested.

During `SingleStep`, eligibility first sets active/enabled/invalid flags. For each
registered transition that is active and invalid at entry, the conflict pass:

1. Visits its input connectors in adjacency order (branch phase), then its output
   connectors in adjacency order (meet phase).
2. At each shared place, counts **active normal connectors**, not unique active
   transitions. It selects whenever that count exceeds one, even if tokens or
   capacity could support all participants.
3. Draws a legacy `RandLong(0, count)` index. It then visits **all** connectors in
   that place's adjacency array, clears each referenced transition's invalid
   flag and disables active transitions whose raw array index differs from the
   draw. The index is not an index into the filtered active-normal candidates.
4. Continues the current input/output loops even if an earlier conflict has
   already disabled the current transition. The active/invalid entry guard is
   checked only at the next outer transition iteration.

The invalid flag therefore affects scheduling as well as drawing. A future pure
selector needs internal visitation state independent of presentation flags; simply
iterating shared places once is not equivalent to these ordered loops.

## Added evidence

| Case | Observed result |
| --- | --- |
| Seeded branch, reversed arc creation | Seeds 1, 5000, 5010, 10000 select raw indices 0, 0, 1, 1; reversed adjacency reverses winners (8 configurations) |
| Meet with spare capacity | Only one of two producers fires despite capacity for both |
| Mixed branch kinds | A test/inhibitor arc at index zero can retain its reader while both normal consumers are disabled (2 configurations) |
| Mixed meet kinds | A test/inhibitor output at index zero can retain its transition while both normal producers are disabled (2 configurations) |
| Parallel normal inputs | Two arcs to the same transition count as two candidates; either draw can disable the sole transition and deadlock (2 seeds) |
| Multiple input conflicts | A transition losing its first branch still resolves a second branch and disables one of its peers |
| Branch followed by meet | A branch loser still resolves its output meet and disables a producer |
| Reconnection | Branch and meet winners follow changed adjacency order while exported document order stays unchanged (2 configurations) |

The mixed-kind and parallel-arc cases are tagged `[legacy-defect]`. They record
compatibility evidence, not desirable future behavior. The continuation cases
register only the current transition to isolate that one outer iteration; they
do not establish that a production pre-scan would return that abbreviated list.
All model objects remain owned by the existing RAII fixture.

Seed traces describe the MSVC CRT plus `HRandom.h` formula, not portable random
generation. Seeds are applied after fixture construction because legacy random
objects reseed the process-wide generator. CTest runs this suite serially. The
existing fixture also checks pure eligibility, token effects and timing against
every new legacy step. No simulation worker or animation wait runs.

Disconnected/malformed adjacency remains a separate risk: some conflict loops
dereference endpoints without rechecking `Connected()`. These tests deliberately
use fully connected, valid exported snapshots and do not provoke legacy crashes.

## Verification (2026-10-28)

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases | 111/111 | 111/111 | 111/111 | 111/111 |
| New conflict cases | 8/8 | 8/8 | 8/8 | 8/8 |
| CTest suites | 11/11 | 11/11 | 11/11 | 11/11 |

Win32 uses `build/vs2022-win32`; x64 uses `build/rules-x64`. Existing legacy
warnings remain. GUI startup and manual editor/printing/animation flows were not
retested for this test-only change. Domain sources are unchanged.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^simulation.conflicts$' --output-on-failure
```

## Next smallest implementation step

Add an explicit, typed conflict-topology projection alongside the snapshot,
without activating it in the UI. Preserve incoming/outgoing ArcId order for
every node and validate exact membership, uniqueness, direction and endpoint
agreement. Reject missing/stale references explicitly. Characterize projection
after reconnection and on the sample documents. Do not change archive layout or
pretend document arc order is sufficient.

Then extract an ordered selector with explicit random choices and diagnostic
traces. Decide separately how to expose the known raw-index and parallel-arc
defects as compatibility policy; do not silently fix them during extraction.
Selection still needs comparison against the real pre-scan and multi-step
behavior before composing a full engine or replacing worker lifetime.

## Rollback

Remove the eight `[conflict-characterization]` cases and `simulation.conflicts`
CTest registration, then remove this document and its links. Existing tests,
domain modules and user documents require no rollback.
