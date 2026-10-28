# Synchronous engine and explicit reset semantics

`src/cpp/simulation/SimulationEngine.*` introduces the separate simulation layer.
The standard C++20 `hps_simulation` target links `hps_domain`; it has no MFC,
Win32, worker or UI dependency. The legacy application does not
use it yet.

## Ownership and creation

`SimulationEngine::Create(snapshot, topology, clock)` returns either an engine
value or structured validation issues. Creation validates snapshot/topology,
legacy eligibility ranges and timing settings without executing a step or
requiring samples for ready stochastic transitions. Initial time/step limit flags
use strict `>`; deadlock is determined only by an executed step.

The engine owns independent initial/current snapshots and topology. `Snapshot()`
returns an independent `EngineSnapshot { net, clock, stop }` value. Changing
caller inputs, returned snapshots or returned step plans cannot change the
engine. The supplied initial snapshot may represent a saved or partially run
state; it is not implicitly reset or normalized.

The timing setting/range checks were factored from `PlanLegacyTiming` into
`ValidateLegacyTimingSettings`, used by both creation and step planning. Snapshot
invariants remain a separate prerequisite. This helper neither advances counters
nor validates random samples. Existing timing tests continue to cover its rules.

## Step and commit boundary

`Step(random)` delegates to the [complete pure step](simulation-step.md). Success
returns the complete plan and commits its snapshot, clock and stop flags together.
Failure returns issues and retains every current field, including previous stop
flags. A caller can correct explicit random inputs and retry the same state.

The candidate state is copied before commit; its move assignment is required to
be non-throwing by a compile-time assertion. Allocation exceptions propagate
without broad catches, with the current state unchanged before commit. This
transactional ownership boundary is synchronous; it is not a cross-thread atomic
operation or publication mechanism.

Step is an explicit manual operation, including after stop flags. It does not
silently refuse or auto-run another step. A future run controller must honor stop
flags when scheduling work. Run/pause, cooperative cancellation and seeded random
generation are not implemented in this stage; random inputs remain explicit.

## Two different reset operations

`ResetLegacy()` reproduces document reset semantics from `CHPosition::Reset`,
`CHTransition::Reset` and `CHSimulation::Reset`:

- Restore each place's configured `initialTokens`, which can differ from the
  current marking supplied at engine creation; clear produced-token counters.
- Clear consumed-token counters and active/enabled flags for every transition.
- Reset Immediate remaining delays to configured initial delays. Retain current
  FixedDelay, Uniform and Exponential remaining delays, preserving the known
  timed-reset defect rather than silently correcting it.
- Clear logical time, step count and stop flags. Preserve sample interval,
  time/step limits, graph definitions and topology.

`RestoreInitialSnapshot()` instead restores the exact engine-creation snapshot,
clock and derived initial stop flags, including its marking, counters, delays
and activity flags. It performs no document reset and draws no random values.
Both operations prepare an independent candidate before committing it. Their
different names make the compatibility decision explicit for a future adapter.

## Tests and verification (2026-10-28)

Twelve portable cases cover creation without a speculative step, invalid topology/
ranges/settings, ownership and snapshot isolation, repeated steps/deadlock, late
sample failure/retry, preservation of previous simultaneous stop flags, returned
plan isolation, timed and immediate reset semantics, exact initial restoration,
independent-engine replay and explicit stepping after stop flags. Headers are
checked for accidental MFC/Windows imports.

Three legacy differential cases compare engine snapshots with the real document
after steps and resets: all four time modes; two weighted-chain runs separated
by reset; and a failed stochastic step followed by a successful seeded retry.
Successful steps also execute the existing complete-step differential checks.
The test fixture prepares the real pre-scan/flags without starting `Online()` or
its worker. Complete online initialization and thread lifetime remain unverified.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, libraries and test build | Passed | Passed | Passed | Passed |
| Previous cases | 176/176 | 176/176 | 176/176 | 176/176 |
| Portable engine cases | 12/12 | 12/12 | 12/12 | 12/12 |
| Engine differential cases | 3/3 | 3/3 | 3/3 | 3/3 |
| CTest suites | 21/21 | 21/21 | 21/21 | 21/21 |

Win32 uses `build/vs2022-win32`, x64 uses `build/rules-x64`. Both domain and engine
libraries also build without MFC in `build/rules-domain-only` (Release). New
targets use `/W4 /WX /permissive-`; existing legacy warnings remain visible.
Production legacy sources, serialization and sample files are unchanged. GUI
startup, editing, printing and animation were not retested for an unused module.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^simulation\.engine' --output-on-failure
```

## Next boundary and rollback

The subsequent [seeded simulation](seeded-simulation.md) adds a per-engine stream,
automatic input planning and transactional random-state ownership while retaining
explicit stepping. Next, add scheduling and cooperative background lifetime.
Snapshots are currently value-isolated,
not safe for concurrent access; an owning controller must serialize access.
Do not activate the engine in the editor before those boundaries are verified.

Rollback removes `src/cpp/simulation`, its root CMake entry/link, engine test
target/file and its two CTest registrations, and fixture engine helpers/cases.
Inline the timing-settings helper back into `PlanLegacyTiming` if desired; its
rules are unchanged. Prior pure-step modules can remain. No user document
migration or production UI rollback is required.
