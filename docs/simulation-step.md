# Complete synchronous simulation step

`PlanLegacySimulationStep` in `src/cpp/domain/SimulationStep.*` composes the
verified structural scan, eligibility/conflict selection, token effects and
timing into one standard C++20 operation. It takes an immutable `NetSnapshot`,
validated endpoint topology, `StepClock` and `StepRandomInputs`. The production
UI does not call it yet. No MFC, Win32, worker, clock service or RNG is used.

## Inputs and result

`StepRandomInputs` contains final integer conflict choices and final sampled
delays keyed by transition ID. The existing strict phase contracts apply: exactly
the encountered choices and selected stochastic samples must be supplied. No
random draws are silently generated or ignored. Scan order is derived from the
topology; callers no longer provide an abbreviated conflict list for this API.

`SimulationStepPlan` contains:

- A complete independent `snapshotAfter`: original graph definitions, new
  marking/produced counters, consumed counters, remaining delays and active/
  enabled transition flags.
- `clockAfter` and independent deadlock/time-limit/step-limit flags.
- The structural conflict list and ordered selection decision trace.
- `movedTokens`, including normal transfers whose net marking change is zero.

Graph definitions and topology are not edited. Keep the original topology for
the next step over `snapshotAfter`; no topology copy is returned. The result
contains no pointers into input or legacy state. It models simulation state,
not geometry, animation events, UI invalid flags or the full legacy log stream.

All phases use the same pre-step snapshot. In particular, timing uses the
original marking/flags plus the chosen selection, not already updated token
values. Joint token effects are validated before returning a successful result.
Only after every phase succeeds is the final snapshot assembled. Errors return
structured issues without a partial result or any input mutation, including
late failures in timing after token effects have been planned.

Stop flags describe the completed step. The function does not prevent a caller
from requesting another step after a limit or deadlock; scheduling and stop
policy belong to the future engine. Reset and run/pause are not implemented here.

## Compatibility and limits

The [selection policy](conflict-selection.md), [token safety range](token-effects.md)
and [timing rules](timing.md) remain unchanged. This preserves known legacy
raw-index/parallel-arc defects; it does not silently correct Petri net semantics.
Malformed/incomplete topology and unsafe arithmetic return errors rather than
emulating invalid legacy behavior. There is no write-back adapter or archive
migration in this stage.

The implementation deliberately reuses validated phase APIs, so some validation
and token-effect planning is repeated. This is a correctness baseline, not a
performance claim for fast simulation. Optimization requires profiling and must
preserve the independent phase tests.

## Differential evidence

`BeginStepComparison` in the existing friend fixture runs the actual legacy
pre-scan once and clears active/enabled flags, matching that part of `Online()`
without creating its worker. The pure initial snapshot is exported afterward.
Full online initialization is still outside the test boundary.

`CheckFullStep` predicts the complete result **before** calling the real legacy
step. It compares all graph-definition fields, every marking/statistics value,
every transition delay/flag, clock settings/counters, all stop flags and conflict
order. Subsequent pure steps consume their own previous snapshot/clock, not a
new export of the kernel's state. Known seeded choices and delay samples are
supplied independently, not read from the kernel result.

Four dedicated cases cover a weighted chain through deadlock (normal and fast),
seven competing-timer steps with alternate conflict winners, seeded Uniform and
Exponential firing/countdown sequences, and mixed test/inhibitor raw-index policy
with simultaneous time/step limits in normal and fast mode. They reuse the
existing per-phase comparisons as additional checks. No simulation worker or
animation wait runs.

Eleven pure cases cover complete field assembly and independent ownership,
repeated stepping, fixed countdown/reset, derived conflicts and explicit choices,
token-statistics overflow, late sample/clock failures, unexpected randomness,
strict/simultaneous stop flags, empty nets, invalid topology and stochastic replay.
Headers are checked for accidental MFC/Windows imports.

## Verification (2026-10-28)

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases | 161/161 | 161/161 | 161/161 | 161/161 |
| Pure complete-step cases | 11/11 | 11/11 | 11/11 | 11/11 |
| Complete-step differential cases | 4/4 | 4/4 | 4/4 | 4/4 |
| CTest suites | 19/19 | 19/19 | 19/19 | 19/19 |

Win32 uses `build/vs2022-win32`, x64 uses `build/rules-x64`. The domain-only
Release library also builds in `build/rules-domain-only`. New domain/test code
uses `/W4 /WX /permissive-`; existing legacy warnings remain. Production
`HSimulation.*`, serialization and sample documents are unchanged in this stage.
GUI startup, editing, printing, animation and background lifecycle were not
retested for this unused module.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^domain\.(simulation-step|step-differential)$' --output-on-failure
```

## Next boundary and rollback

Add a synchronous engine that owns validated initial/current state and commits a
successful step as a whole, with explicit reset semantics and error handling.
Keep random inputs explicit until a separately tested per-engine seeded RNG
boundary exists. Run/pause scheduling, cooperative background shutdown,
thread-safe publication and production UI integration follow as separate stages.

Rollback removes `SimulationStep.*`, its source entry, `SimulationStepTests.cpp`,
the two CTest registrations, and full-step fixture helpers/four cases. Existing
scan/selection/effects/timing APIs can remain. No user data conversion or
production UI rollback is required.
