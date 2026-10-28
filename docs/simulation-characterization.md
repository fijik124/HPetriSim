# Legacy simulation characterization

The 23 simulation-tagged Catch2 cases in `tests/SimulationCharacterization.cpp` execute the actual
`CHSimulation::SingleStep`, model classes, reset code and notification handler.
They do not implement a second simulation algorithm. The only production change
for this stage is a friend declaration in `HSimulation.h`; rules, public APIs,
thread code and serialization remain unchanged.

## Test boundary

A console main initializes MFC without calling the application's `InitInstance`.
Each fixture owns its places, transitions and arcs with `unique_ptr`; document
and simulation collections borrow these objects. Connections and aliases are
removed before destruction. A message-only window records notifications and
passes them to the real `OnSimThreadMessage` handler, without a visible frame or
document views. No simulation worker, timer, printer discovery or file I/O is used.

The fixture registers model pointers directly. It intentionally bypasses
`Online()` and its thread creation and conflict pre-scan. Conflict tests explicitly
register their candidates. The tests therefore characterize synchronous steps,
not the entire online/run/pause/shutdown state machine.

Normal-mode tests use simulation speed 1 to avoid entering the animation wait.
They establish marking equivalence with fast mode at that speed, not animation
correctness or thread safety.

## Recorded behavior

| Area | Expectations checked |
| --- | --- |
| Normal arcs | Weighted consumption/production; place and transition counters |
| Input shortage | No marking change; deadlock; step counted, time unchanged |
| Capacity | Exact fit allowed; overflow blocks the transition |
| Test input | Threshold required, tokens not consumed |
| Inhibitor input | Strict `tokens < weight`, tokens not consumed |
| Test output | Capacity checked, tokens not produced |
| Inhibitor output | At capacity, firing still consumes input without producing output |
| Fixed delay | Countdown by sample interval; firing on a subsequent step; restart after firing |
| Disabled timer | Fixed delay returns to its initial value |
| Limits | Equality is allowed; stopping happens after exceeding time/step limits |
| Fast/normal | Equal marking, step/time counters; notifications delivered |
| Reset | Initial marking restored; statistics, time and steps cleared |
| Empty graph | Deadlock on step 1 |
| Sample interval | Overshoot yields a negative signed delay; firing on the next step |
| Immediate traffic | Normal token transfers prevent timed countdown in the same step |
| Uniform delay | Seed 1, range [2, 7): recorded resamples `2, 4, 2, 6`; replay matches |
| Exponential delay | Seed 1, parameter 10: recorded resamples `5, 10, 7, 13` |
| Branch conflict | One candidate fires, even if tokens could support both |
| Meet conflict | Shared output capacity respected by selecting one producer |

The random traces describe the current MSVC CRT and legacy formula. Tests call
`srand(1)` after constructing the fixture because `CHRandom` constructors reseed
the process-wide RNG. This is a test-only replay mechanism, not a new production
seed API, independent RNG per document, or a cross-platform randomness contract.
Random cases run serially.

## Known defects are evidence, not the intended future contract

Three cases carry `[legacy-defect]`:

1. Resetting a timed transition retains its remaining delay rather than its
   configured initial delay.
2. A full-capacity normal self-loop is blocked because output capacity is checked
   before input consumption.
3. In a branch with an inactive first arc and two active candidates, seed 1 can
   select the inactive arc index and disable both active candidates. The result
   is a deadlock although a firing was available.

Other unusual policies are tagged `[legacy-policy]` without assuming they are
necessarily bugs. In particular, time advancement and the meaning of output
inhibitor arcs need an explicit semantics decision before a replacement engine
changes them. The legacy exponential formula should not be silently replaced
with a standard distribution during extraction.

For a future correction, keep the minimal reproducer, document the new expected
behavior and update that expectation in a dedicated change. Do not distort a new
domain model solely to retain a confirmed defect.

## Run and rollback

```powershell
cmake --preset vs2022-win32
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R simulation --output-on-failure
```

The console tests use MFC and Windows and currently link the legacy application
sources. They are a temporary harness, not the future portable domain library.
They can run without the optional desktop startup tests. After a build, run the
test executable with `[legacy-defect]` to select the confirmed defect cases.

Rollback consists of removing the simulation test target/files and the friend
declaration. No user documents need conversion or rollback.

## Verification (2026-10-28)

| Configuration | Application build | Simulation cases | Existing archive cases |
| --- | --- | --- | --- |
| Win32 Release | Passed | 23/23 | 6/6 |
| Win32 Debug | Passed | 23/23 | 6/6 |
| x64 Release | Passed | 23/23 | 6/6 |
| x64 Debug | Passed | 23/23 | 6/6 |

x64 checks used the existing isolated `build/verify-x64` directory. Desktop
startup tests were not repeated for a friend-only production-header change.
The initial test build required correcting const qualifiers in the fixture and
including `resource.h`; no production API change was needed for those errors.

## Remaining coverage before threading changes

- Online graph indexing, incomplete connectors and conflict pre-scan.
- Real background run/pause/reset/stop and document closure during work.
- Animation phases, cancellation and notification lifetime across threads.
- Multi-document RNG interference and input validation/overflow.
- Save/load of a running or paused state and historical unversioned fixtures.

The subsequent [eligibility extraction](simulation-rules.md) compares pure
pre-conflict decisions with each legacy step in this fixture. Its results remain
independent of MFC drawing state and notification delivery. The subsequent
[token-effect extraction](token-effects.md) compares marking and statistics after
each step using the selection supplied by the legacy kernel. The subsequent
[timing extraction](timing.md) compares delays, logical time, step counts and stop
reasons using explicit stochastic samples. Conflict policy and background lifetime
still need extraction and verification.
