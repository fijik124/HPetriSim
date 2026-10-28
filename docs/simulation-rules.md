# Pure legacy eligibility rules

`EvaluateLegacyEligibility` in `src/cpp/domain/SimulationRules.*` evaluates the
first phase of the legacy step over a `const NetSnapshot&`. It does not call MFC,
Win32, GDI+, clocks or random generators. It neither edits the snapshot nor
changes the application backend.

## Result and contract

The result is a variant containing either a complete ordered decision list or
validation issues. Each transition has exactly one decision:

- `ArcBlocked`: a blocking arc ID and reason.
- `DelayPending`: the positive remaining delay from the input snapshot.
- `FireCandidate`: arc conditions pass and remaining delay is zero or negative.

A candidate is not a promise to fire: conflict selection happens later. A pending
delay is not a promise of time advancement. The legacy engine's time/deadlock
logic, including immediate transitions with positive stored delays, remains
outside this evaluator.

All transitions observe the same initial marking. The evaluator does not apply
hypothetical earlier firings to enable later transitions in the same pass.
Decisions preserve transition order. Internal arc indices avoid repeatedly
scanning the entire graph for every transition. Blocking diagnostics select the
first failing input arc in snapshot order, then the first failing output arc.
Legacy reconnection can reorder adjacency lists independently of document order;
the tests compare eligibility flags, not an identical first diagnostic in every
edited graph. Conflict-order extraction remains separate work.

## Compatibility predicates

| Arc direction/kind | Condition to pass |
| --- | --- |
| Input normal or test | `tokens >= weight` |
| Input inhibitor | `tokens < weight` |
| Output normal or test | `capacity - tokens >= weight` |
| Output inhibitor | Legacy `(tokens - capacity) < weight` |

The last predicate is intentionally preserved. For a validated marking at or
below capacity and positive weight it always passes, even at full capacity.
The implementation guards unsigned subtraction rather than introducing
underflow. Full self-loops also retain their legacy behavior: output capacity
is checked before consuming input tokens.

Delay classification uses the stored remaining delay in every timing mode,
matching the legacy eligibility phase. There is no countdown, timer reset,
uniform/exponential resampling, token consumption, statistics update, animation,
limit handling or notification delivery here.

Structural validation runs first. For compatibility, current tokens, capacities
and weights must also fit the signed 32-bit range used by legacy getters;
remaining delay must fit `int32_t`. Other valid domain snapshots can retain wider
counts, but this compatibility evaluator returns `UnsupportedLegacyRange` rather
than silently truncating or emulating signed overflow. This range check concerns
the fields read by eligibility, not all arithmetic a future full engine needs.

## Verification

Fifteen pure Catch2 cases cover thresholds, weights, capacity, every timing mode,
negative delays, self-loops, deterministic ordering, shared input marking,
invalid snapshots and signed boundaries. Header guards check that the rule
headers do not import Windows or MFC headers.

Seven differential cases run the actual `CHSimulation::SingleStep`:

| Coverage | Configurations |
| --- | --- |
| Input kind, token count, weight, delay | 90 |
| Output kind, marking, weight, delay | 90 |
| Mixed input/output conjunction | 3 sections |
| Every timing mode and signed delay | 16 |
| Pre-conflict candidates versus selected firing | 1 |
| Maximum signed marking | 1 |
| Chained transitions | 1 net over 2 steps |

The existing 23 characterization cases also compare pure decisions before their
legacy steps. The fixture now includes arcs in its borrowed document list so the
real legacy adapter can construct complete snapshots. Without conflict candidates,
the post-step `Activ`/`Enabled` flags are compared exactly. With conflicts, blocked
and waiting decisions are checked, while a firing candidate may be deselected by
the original conflict code. A dedicated test shows two candidates becoming just
one selected firing. These checks do not claim conflict-algorithm equivalence.

```powershell
cmake --preset vs2022-win32 -DBUILD_TESTING=ON
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(domain\.|simulation\.|persistence\.)' --output-on-failure
```

No desktop startup tests are needed for this stage: the new evaluator is not
called by the UI or legacy worker. The existing `.hps` files and production
`HSimulation.cpp` are unchanged. A separate domain-only build verifies the rules
compile with `HPETRISIM_BUILD_MFC_APP=OFF` and `BUILD_TESTING=OFF`.

## Build and test results

Verification on 2026-10-28:

| Check | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Existing model/adapter/simulation/archive cases | 49/49 | 49/49 | 49/49 | 49/49 |
| Pure rule cases | 15/15 | 15/15 | 15/15 | 15/15 |
| Differential cases | 7/7 | 7/7 | 7/7 | 7/7 |

Win32 used the recreated `build/vs2022-win32`; x64 used `build/rules-x64` so the
existing IDE build directory was left alone. The first build attempt found that
the old Win32 directory no longer existed; reconfiguration restored it and the
pinned Catch2 dependency. `build/rules-domain-only` also built the new library
successfully without MFC. No Linux build or performance benchmark was performed.

## Rollback and next boundary

Remove `SimulationRules.*`, its domain source entry, the range diagnostic code,
the new test file/CTest entries and the test-fixture comparisons/differential
cases. No persistence migration or reverse adapter is required.

The subsequent [token-effect extraction](token-effects.md) plans marking and
statistics for explicitly selected transitions and compares them with legacy
results. Conflict selection, random sampling, time advancement and safe
background execution should remain separate, testable steps before activating
the new implementation in the UI.
