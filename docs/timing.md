# Pure delay and logical-time planning

`PlanLegacyTiming` in `src/cpp/domain/Timing.*` takes an immutable net snapshot,
selected transition IDs, a `StepClock` and explicit stochastic delay samples. It
returns either a `TimingPlan` or structured validation issues. It uses no MFC,
Win32, wall clock, random generator or worker. The production UI does not call it
yet, and the production simulation kernel and archive format are unchanged.

## Inputs and resulting state

`StepClock` carries logical time, step count, sample interval and time/step limits.
The result copies these settings, advances the counters according to legacy
rules, and returns each transition's remaining delay and active/enabled flags.
Independent stop flags represent deadlock, time limit and step limit;
`Locked()` combines them. Marking and statistics remain the responsibility of
[token-effect planning](token-effects.md).

Samples are final resampled delays, not raw random draws. Exactly one sample is
required for each selected Uniform or Exponential transition. Missing samples,
extra samples, negative values and values beyond the supported range are errors.
Uniform samples must lie in the configured half-open interval `[min, max)`.
Exponential samples may include zero; their probability distribution is not
validated or generated here. Conflict selection is supplied by the caller.

## Preserved behavior

- Every successful planned step increments the step count, including deadlock.
- A transition is enabled when its arcs permit firing but its delay is pending;
  active means selected to fire. These are distinct flags.
- A previously enabled fixed-delay transition restarts its delay when it stops
  being enabled, including when it becomes blocked or loses a conflict after
  reaching zero. Other disabled timers retain their delay.
- Firing resets fixed delays and resamples stochastic delays. Immediate
  transitions retain their stored delay. A fired timer is not decremented in
  that same step.
- Any normal token transfer freezes logical time and pending timers for that
  step. Transfers around a self-loop still count even with no net marking change.
- Without normal transfers, pending non-immediate timers decrease by the sample
  interval and permit logical time to advance. Signed countdown overshoot is
  retained. A selected read-only or isolated transition also permits time to
  advance even when no timer is pending.
- A positive stored delay on an Immediate transition does not count down and
  does not by itself prevent deadlock.
- Time and step limits use strict `>`, not `>=`. All three stop reasons can
  coexist; reaching one does not suppress the others.

These rules preserve observed behavior for migration. They do not redefine Petri
net semantics or fix the separately documented legacy conflict/reset defects.

## Validation and safe range

Token planning first validates the snapshot, eligibility and joint selected
effects. Timing then validates its settings and samples. Clock values and limits
must fit the nonnegative signed 32-bit legacy range; the sample interval must be
positive. Initial delays and uniform upper bounds must also fit that range.
Step/time additions that would exceed it return `ArithmeticOverflow` instead of
emulating legacy overflow. Remaining delays can overshoot below zero during a
valid countdown. All calculation uses local values; an error returns no partial
plan and leaves the input unchanged.

This is an explicit safe compatibility subset. It does not support malformed
clock settings, arbitrary large timing configurations or unsafe legacy arithmetic.

## Characterization and differential evidence

Eighteen pure cases cover countdown, overshoot, fixed restart, disabled timers,
resampling, transfer-dependent time advancement, immediate delays, sample/clock
validation, strict limits, simultaneous stop reasons and arithmetic boundaries.

The synchronous legacy fixture now compares timing on every step: logical time,
step count, all transition delays/flags and stopping behavior. It exports the
snapshot before the real step. Selection and final stochastic samples come from
the legacy result as external inputs; expected timing values are calculated from
the original snapshot. These comparisons isolate timing from conflict selection
and random generation, and do not independently validate either of those phases.

Four dedicated differential cases cover competing fixed timers (including the
loser's restart), read-only firing alongside a pending timer, seeded stochastic
replay, and simultaneous stop reasons. The replay case supplies known Uniform
and Exponential sample sequences to the planner **before** executing the legacy
step, then compares results. It verifies replay against that seeded trace, not
the statistical quality of a new random generator. No simulation worker runs.

## Verification (2026-10-28)

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases | 89/89 | 89/89 | 89/89 | 89/89 |
| Pure timing cases | 18/18 | 18/18 | 18/18 | 18/18 |
| Dedicated timing differential cases | 4/4 | 4/4 | 4/4 | 4/4 |
| CTest suites | 10/10 | 10/10 | 10/10 | 10/10 |

The domain-only Release library build also passed. Win32 uses
`build/vs2022-win32`, x64 uses `build/rules-x64`, and the domain-only build uses
`build/rules-domain-only`. Existing legacy compiler warnings remain visible.
Sample files are unchanged. GUI startup, printing, animation and background
lifetime were not retested for this unused domain module.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(domain\.|simulation\.|persistence\.)' --output-on-failure
```

## Rollback and next boundary

Remove `Timing.*`, its domain source entry, the two new validation codes,
`TimingTests.cpp` and timing CTest entries, and the fixture's timing comparisons
and four dedicated cases. Existing eligibility/token-effect planning can remain.
No user document conversion or production UI rollback is needed.

Next, characterize and extract conflict selection with explicit random inputs,
keeping the known selection-index defect visible as a compatibility decision.
Only then compose these phases into a complete deterministic step. Full reset,
random generation, a background engine, cooperative shutdown, thread-safe
publication and UI integration remain separate work; the existing unsafe worker
shutdown has not been replaced by this stage.
