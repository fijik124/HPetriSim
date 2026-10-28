# Owning background simulation service

`BackgroundSimulation` in `src/cpp/simulation/BackgroundSimulation.*` owns a
`RunController` and one `std::jthread`. After construction, only that worker
accesses the controller. It has no MFC/Win32, document pointer, UI callback,
`SendMessage`, timer or forced thread termination. The legacy application does
not use it yet; its original worker still requires replacement.

## Commands and publication

`Submit(ServiceCommand)` accepts Resume, Pause, Stop, ResetLegacy and
RestoreInitialSnapshot through a mutex-protected FIFO. Each command returns a
future with its outcome and immutable acknowledgement snapshot. The queue is
bounded (default 64); saturation throws `overflow_error`, invalid commands or
zero configuration values throw `invalid_argument`. No command is silently dropped.

Resume acknowledges Applied, ResetRequired or SimulationStopped according to the
controller. While Running, the worker repeatedly executes bounded seeded batches
(default 16 successful steps per batch). It checks queued commands before starting
the next batch. Paused, Stopped and Failed states wait on a condition variable.
Existing pause/stop/reset and known legacy policies are unchanged.

Pause/Stop submissions also signal the current batch's cooperative token; reset
submissions signal pause before applying reset on the owner thread. Tokens are
recreated for each batch. A delayed signal for an old batch cannot cancel a later
resume. No engine operation is interrupted halfway through a step.

`PublishedSimulation` includes revision, run state, complete engine snapshot/RNG,
last batch/control end reason, diagnostics and a closed flag. Construction,
commands, batches and final closure publish independent immutable values through
`atomic<shared_ptr<const PublishedSimulation>>`. This guarantees coherent ownership
and visibility, not a lock-free implementation. Publication revisions increase;
they are not simulation step counts.

`Snapshot()` returns a shared immutable handle. Readers may retain it after newer
publications or service destruction. `WaitForUpdate(revision, timeout)` waits for
a newer publication or worker completion; timeout can return the same revision.
Command futures acknowledge that command's serialized execution point. A later
queued Resume may start work before a caller examines an earlier Pause reply.

## Shutdown and failures

`Shutdown()` marks closure, signals the current batch and jthread, wakes an idle
worker, and joins without holding the command mutex. Join callers are serialized.
The worker finishes any current step/command, stops, publishes a final closed
snapshot and settles remaining queued futures with Closed rather than applying
them. Submissions after closure return Closed with the latest publication;
during closure that publication may precede the final state. Shutdown completion
or the completion future establishes that the worker has finished.

The non-copyable/non-movable owner keeps the worker's captured service reference
valid. Its destructor requests stop and joins before controller, synchronization
objects or snapshots are destroyed. Callers must finish their own service method
calls before destroying the object. Retained snapshots and completion futures
have independent lifetimes; they do not keep the service worker running.

Domain errors publish Failed state, original diagnostics and the last successful
engine state. The worker remains available for an explicit reset. Stop does not
erase failure diagnostics. Such errors are not exceptional worker termination.

Unexpected exceptions propagate through a `packaged_task` completion future.
There is no application catch-all or fallback-to-success handler. A scope guard
wakes readers, closes submissions and destroys outstanding promises on exit, so
pending command futures report broken promises if unexpected failure prevented
a reply. `Snapshot`, `WaitForUpdate` and `Submit` observe completed worker failure
by rethrowing its completion exception. `Shutdown()` joins and rethrows it too.
Use explicit Shutdown at a controlled close point to report failures; the
destructor only joins. A saved `Completion()` shared future remains observable
after destruction. Tests do not inject allocation or OS thread failures.

## Verification (2026-10-28)

Fourteen portable cases cover idle waiting, bounded seeded trajectory equivalence,
pause/resume, terminal stop/reset, restoring during a run, domain and sampled-delay
failure publication, retained snapshots, destruction while idle/running, waiter
wakeup, pending-future settlement, concurrent shutdown callers/readers/producers,
and configuration/command rejection. Concurrent reader tests synchronize startup
with a latch; futures and condition waits use bounded deadlines instead of sleeps.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, libraries and test build | Passed | Passed | Passed | Passed |
| Previous cases | 223/223 | 223/223 | 223/223 | 223/223 |
| Background service cases | 14/14 | 14/14 | 14/14 | 14/14 |
| CTest suites | 26/26 | 26/26 | 26/26 | 26/26 |
| Additional background suite repetitions | 5/5 | 5/5 | 5/5 | 5/5 |

Win32 uses `build/vs2022-win32`; x64 uses `build/rules-x64`. Domain and simulation
libraries also build without MFC in `build/rules-domain-only` Release. The new
target uses CMake `Threads::Threads` and strict MSVC warnings; legacy warnings
remain visible. No race sanitizer or exhaustive scheduler proof is claimed.
No production legacy source, archive or sample changed. MFC window/document-close,
printing, animation and real UI notification races remain integration work.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^simulation\.background$' --output-on-failure
```

## Next boundary and rollback

The subsequent [document session](document-session.md) creates services from
validated offline exports with tagged identities, owner-thread controls,
presentation values and explicit closure. MFC edit/close event binding and result
application remain separate work. Cancellation latency is bounded by current step
completion, not a fixed time guarantee.

Rollback removes `BackgroundSimulation.*`, its source/Threads link, its test file
and CTest entry. Controller/engine APIs and documents remain unchanged. Existing
legacy worker replacement and UI activation are separate stages.
