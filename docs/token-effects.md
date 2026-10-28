# Pure token-effect planning

`PlanLegacyTokenEffects` in `src/cpp/domain/TokenEffects.*` takes a snapshot and
explicitly selected transition IDs. It returns either an independent
`TokenEffectPlan` or structured validation issues. It never changes the input,
calls a random generator, updates a timer, accesses a window or starts a worker.
The editor and production simulator do not call it yet.

## Planned state

The plan contains the resulting marking (including each place's produced-token
statistics), consumed-token statistics for all transitions, and selected IDs in
the original net's transition order. Unselected data is retained. It does not
return a complete post-step snapshot: delays, active/enabled flags, simulation
time, step counts and stop reasons are outside this phase.

`movedTokens` means that a normal input/output arc transferred tokens. It is true
for a self-loop whose final marking is unchanged, and false when selected
transitions have only test/inhibitor arcs or no arcs. This matches the information
needed by the legacy time-advancement condition, without implementing that
condition here.

## Order and checks

1. Validate the snapshot and legacy eligibility range.
2. Reject unknown, duplicate, blocked or waiting selected IDs.
3. Consume every selected normal input before producing any normal output.
4. Increment transition statistics by consumed weights, not firing count.
5. Produce normal output weights and increment the receiving place's statistics.

Test and inhibitor arcs keep their eligibility conditions but have no token or
statistics effects. Their conditions use the original marking, not a marking
already reduced by another input arc. Transition selection is the caller's job;
the planner does not reproduce legacy branch/meet conflict selection.

Each selected transition can pass eligibility individually while the batch is
unsafe. Shared input shortages return `TokenUnderflow`; combined output capacity
violations return `CapacityExceeded`. Input shortage is checked before outputs
can restore tokens, so a zero net delta cannot hide invalid consumption. Invalid
selections return `InvalidSelection`.

Increments to statistics must fit the nonnegative signed 32-bit legacy range.
An unsafe addition returns `ArithmeticOverflow`, including sums of input weights
which would overflow the legacy local `int fired` accumulator. No signed/unsigned
wraparound is emulated. Untouched statistics are copied without narrowing even
when wider than that range. This is a safe compatibility subset, not a promise
to emulate every behavior of old out-of-range counters.

All work is on local values. Any failure returns diagnostics only, never a
partially successful plan or a modified source snapshot. Successful token totals
are computed by complete input/output phases. This API does not specify per-arc
animation or logging event order; those need a separate extraction.

## Differential evidence

The existing synchronous test fixture exports the snapshot before the real legacy
step. After that step it takes only the selected IDs from `Activ()` flags and
asks the pure planner to predict token effects from the original snapshot. It
compares every place's current/produced tokens and every transition's consumed
tokens. The expected token values are not copied from the legacy result.

These comparisons run throughout the existing characterization and eligibility
differential suites, including seeded conflict winners and chained transitions.
Three additional cases cover weighted multiple inputs/outputs in both fast and
normal mode, a joint source/sink step and a safe shared-place batch. The last
case deliberately bypasses `Online()` conflict preprocessing to test the actual
token-transfer phase for a supplied selection; it does not claim the legacy
conflict policy would choose that whole batch.

Fifteen pure cases additionally cover immutability, identity effects, read and
inhibitor arcs, selection validation, joint failures, statistics boundaries,
self-loops, canonical selection order and preservation of untouched data.
Unsafe legacy arithmetic is tested on the pure planner, not executed in the
legacy kernel merely to observe wraparound or undefined behavior.

## Verification (2026-10-28)

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases | 71/71 | 71/71 | 71/71 | 71/71 |
| Pure token-effect cases | 15/15 | 15/15 | 15/15 | 15/15 |
| Dedicated effect differential cases | 3/3 | 3/3 | 3/3 | 3/3 |

The domain-only library build also passed. Win32 uses `build/vs2022-win32`, x64
uses `build/rules-x64`, and the domain-only build uses `build/rules-domain-only`.
Legacy warnings remain visible. Neither `HSimulation.cpp` nor the sample files
changed; GUI startup checks were not repeated for an unused pure-domain module.

```powershell
cmake --preset vs2022-win32 -DBUILD_TESTING=ON
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(domain\.|simulation\.|persistence\.)' --output-on-failure
```

## Rollback and next boundary

Remove `TokenEffects.*`, its domain source entry, new diagnostics, test file/CTest
entries and the fixture's effect comparisons/new cases. No user data conversion,
reverse adapter or UI rollback is needed.

The subsequent [timing extraction](timing.md) covers delay updates and logical-time
advancement with explicit samples for timed resampling. Conflict policy, a full
engine, background lifetime, thread-safe publication and UI activation remain
separate stages. The existing unsafe worker shutdown has not been changed by
this token-planning step.
