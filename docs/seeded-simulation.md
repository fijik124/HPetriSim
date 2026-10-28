# Per-engine deterministic random input

`SeededStep.*` in `src/cpp/simulation` adds a versioned legacy-compatible stream
and automatic input planning. `SimulationEngine::Create(..., clock, seed)` accepts
an explicit 32-bit seed (default 1). `StepSeeded()` generates conflict choices and
selected stochastic delays, executes a complete step and commits model/clock/stop
and random state together. The production UI still uses its original simulator.

## Stream and sampling contract

Version 1 uses unsigned 32-bit arithmetic:

```text
state = state * 214013 + 2531011   (modulo 2^32)
draw  = (state >> 16) & 32767
```

The recurrence is explicit. `LegacyRandomState` holds its current word;
`LegacyRandom` advances an owned value. Creation does not reseed or consume CRT
state, wall time or another engine's stream. Zero and full-width seeds are valid.

Inputs follow legacy execution order: one draw per encountered conflict, then
one draw per selected Uniform/Exponential transition in net order. Waiting,
blocked, deterministic and unselected transitions consume no delay draw.
Steps with no random work leave the stream unchanged.

Conflict/uniform mapping preserves `HRandom::RandLong`'s floating calculation and
exclusive upper bound, including remapping the maximum raw draw to the last valid
element. Exponential mapping preserves the legacy `+0.5` logarithmic formula and
intermediate `RandDouble` calculation. Non-finite, negative or unsupported generated
delays return `ArithmeticOverflow` before conversion; no clipping is performed.

Integer outputs are specified by the recurrence. Exponential samples depend on
floating-point/log implementation; tests establish the current MSVC Win32/x64
baseline, not bit-identical results on every future math library. Future portable
persistence should record algorithm version and stream word alongside model
state. This stage does not add a file format.

## Dynamic requests and commit

The domain's `ContinueLegacyConflictSelection` replays a validated choice prefix
and returns a final plan, a typed `ConflictChoiceRequest`, or diagnostics.
Requests include conflict kind, visited transition, place and current active
normal-arc count. Later counts can depend on earlier choices. The simulation
layer answers requests by drawing locally and extending the prefix.

The domain remains free of randomness and callbacks. Missing input is a request
only in the prefix API; malformed or extra choices are errors. The original
strict selector converts a pending request to its existing diagnostic. Prefix
replay repeats validation/work; it is not a fast-path optimization.

`PlanSeededLegacyStep` starts from a local stream copy and returns a
`SeededStepPlan { step, inputs, randomAfter }` only after full success.
`StepSeeded()` commits the full snapshot using the existing non-throwing assignment
boundary. Failure, including after sampling/token planning, leaves model, clock,
stop flags and random word unchanged.

Returned `inputs` replay through the pure step or explicit engine `Step`, which
does not advance the seeded stream. `EngineSnapshot` includes an independent
random-state value. `ResetLegacy()` retains stream position, matching the legacy
reset's lack of reseeding. `RestoreInitialSnapshot()` restores the original seed
word and initial model/clock/stop values. Neither reset draws random values.
The engine remains synchronous and requires serialized ownership.

## Verification (2026-10-28)

Three domain cases cover prefix requests, invalid prefixes and dynamically
changing requests. Ten portable cases cover pinned raw/uniform/exponential
traces, maximum raw draw, conflict-before-delay order, explicit replay, no-draw
steps, numeric/statistics failure rollback, reset/rewind, independent engines,
different seeds and explicit-step stream isolation.

Three legacy cases compare 256 CRT draws for six seeds (1,536 outputs, including
0 and UINT_MAX), verify seeded stepping leaves calling-thread CRT state untouched,
replay changing branch/meet/delay draws through the real kernel, and continue
Uniform/Exponential runs through legacy reset. Legacy suites run serially without
a simulation worker. Existing complete-step/state comparisons run after every
successful differential step.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, libraries and test build | Passed | Passed | Passed | Passed |
| Previous cases | 191/191 | 191/191 | 191/191 | 191/191 |
| New selection prefix cases | 3/3 | 3/3 | 3/3 | 3/3 |
| Portable seeded cases | 10/10 | 10/10 | 10/10 | 10/10 |
| Seeded differential cases | 3/3 | 3/3 | 3/3 | 3/3 |
| CTest suites | 23/23 | 23/23 | 23/23 | 23/23 |

Win32 uses `build/vs2022-win32`; x64 uses `build/rules-x64`. Both libraries also
build without MFC in `build/rules-domain-only` Release. Existing legacy warnings
remain visible. Legacy production sources, serialization and samples are unchanged.
GUI startup, editing, printing, animation and background lifetime were not retested
for modules not activated in the application.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(simulation\.seeded|simulation\.seeded-differential|domain\.conflict-selection)$' --output-on-failure
```

## Next step and rollback

Add a bounded synchronous run controller with explicit stop/pause/error results
before introducing workers and concurrent snapshot publication. Keep replay tests
and legacy policy. Existing unsafe shutdown still needs replacement before
production integration.

Rollback removes `SeededStep.*`, its source/test registrations and three new
legacy cases; restore engine declarations/implementation without random state.
Revert the prefix API/refactor and its three tests together, retaining the original
strict selector. Existing explicit stepping/reset can remain. No data migration
is needed.
