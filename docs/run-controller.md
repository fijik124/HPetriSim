# Bounded synchronous run controller

`RunController` in `src/cpp/simulation/RunController.*` owns a validated seeded
`SimulationEngine`. It starts paused and provides bounded execution, explicit
pause/stop state, diagnostics and independent result snapshots. It uses standard
C++20, without MFC, Win32, a worker, timers or UI callbacks. Production simulation
is unchanged.

## State and results

`Resume()` starts or continues running, returning `Running`. It returns
`ResetRequired` for a stopped/failed controller and `SimulationStopped` when
engine stop flags already prevent automatic execution. No step or draw occurs.

| Controller state | Meaning | Continuation |
| --- | --- | --- |
| Paused | Initial state, explicit pause, or simulation stop | Resume if engine stop flags are clear |
| Running | Ready for the next synchronous batch | Run another batch or pause/stop |
| Stopped | Explicit stop request | Reset or restore initial snapshot before resume |
| Failed | A step returned validation issues | Inspect diagnostics, then explicitly reset/restore |

`RunBatch(stepBudget, requests)` executes at most that many successful seeded
steps in the call. Zero is valid and performs no step. The budget is a count,
not a wall-clock deadline. `RunResult` contains an end reason, successful-step
count, independent final engine snapshot (including RNG state), and diagnostics.
It does not retain an unbounded history of plans.

- `BudgetExhausted` leaves the controller Running so the caller can submit the
  next batch. Partitioning a trajectory into batches does not alter RNG draws.
- `Paused` preserves model/clock/RNG state. Resume continues from that point.
- `Stopped` preserves state but requires an explicit reset before resuming.
- `SimulationStopped` pauses immediately after a successful step reporting
  deadlock, time limit or step limit. Independent engine stop flags retain all
  simultaneous reasons. This takes precedence over an exhausted step budget.
- `StepError` retains every earlier successful step and the failed step's
  diagnostics. The failed step itself changes neither model nor RNG state.

After a simulation stop, another batch returns Paused without work; Resume
reports SimulationStopped until reset. After failure, another batch returns the
stored error with zero completed steps. Pause/Stop do not hide a Failed state.
The batch is not one transaction: successful steps before a failure remain
committed. Allocation exceptions propagate rather than becoming silent fallbacks.

## Cooperative request boundary

`RunRequests` supplies separate `std::stop_token` values for pause and stop, scoped
to each batch call. The external owner may signal their sources concurrently.
Terminal Failed/Stopped states are checked first, then stop requests, pause
requests, paused state and the budget. Stop wins if both tokens are requested.
A pre-requested token can therefore prevent any work even with zero budget.

Requests are sampled between whole steps. A step already executing finishes and
commits before another request check; a completed step reporting simulation stop
returns immediately. There is no forced interruption or guarantee of real-time
cancellation latency. The request tests cover pre-requested tokens and changes
between batches. Actual worker execution/concurrent request timing belongs to
the next stage.

All controller methods and snapshot reads require serialized ownership. The
tokens are the only supported concurrent input. `Pause()` and `Stop()` are
synchronous operations between batches, not thread-safe worker commands.

`ResetLegacy()` and `RestoreInitialSnapshot()` delegate to their distinct engine
operations and then clear diagnostics and set Paused. Legacy reset retains RNG
position and timed remaining delays; initial restoration rewinds to creation
state. If engine reset throws before commit, controller state is unchanged.

## Verification (2026-10-28)

Fourteen portable cases cover initial pause, zero/finite budgets, batching
invariance, pause/resume, terminal stop, request precedence, strict limits,
deadlock, errors after successful progress, generated-delay rollback, reset
semantics, snapshot isolation and initially exceeded limits. Header checks prevent
accidental MFC/Windows inclusion.

Two legacy differential cases replay Uniform/Exponential trajectories through
pause/resume and deadlock, and compare a batch ending at simultaneous limits
against actual synchronous legacy steps and reset. Existing full-step and engine
snapshot comparisons are reused. No legacy simulation worker starts.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, libraries and test build | Passed | Passed | Passed | Passed |
| Previous cases | 207/207 | 207/207 | 207/207 | 207/207 |
| Portable controller cases | 14/14 | 14/14 | 14/14 | 14/14 |
| Controller differential cases | 2/2 | 2/2 | 2/2 | 2/2 |
| CTest suites | 25/25 | 25/25 | 25/25 | 25/25 |

Win32 uses `build/vs2022-win32`, x64 uses `build/rules-x64`. Both libraries also
build without MFC in `build/rules-domain-only` Release. Existing legacy warnings
remain visible. Legacy production sources, archives and sample files are
unchanged. GUI startup, editing, printing, animation and background lifetime were
not retested for this unused module.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^simulation\.controller' --output-on-failure
```

## Next boundary and rollback

Add an owning background service using cooperative cancellation and RAII lifetime,
with serialized controller commands, synchronized value snapshots and explicit
error delivery. Test pause/stop/document-close races and joining without blocking
on UI messages before replacing the unsafe production worker. Keep UI activation
and persistence migration separate.

Rollback removes `RunController.*`, its library source entry, controller test file,
the two CTest registrations and two fixture cases. Existing synchronous engine,
seeded step and reset APIs remain usable. No user data migration is required.
