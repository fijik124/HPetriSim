# Pure legacy conflict selection

`PlanLegacyConflictSelection` in `src/cpp/domain/ConflictSelection.*` selects
transitions from an immutable snapshot, validated endpoint topology, an explicit
ordered conflict list and explicit random choices. It uses standard C++20 and
does not access MFC, Win32, a random generator, a worker or UI state. The
production simulator does not call it yet.

## Contract and compatibility policy

The function validates topology and evaluates legacy eligibility. Only firing
candidates start active. The supplied conflict order may be a subset, including
empty, but cannot contain unknown or duplicate transition IDs. It is an input
boundary: this function does not infer or verify the production `Online()`
pre-scan list. Transition visitation follows this order, while final selected
IDs are returned in `PetriNet::transitions` order.

Choices are final integer results of the legacy `RandLong(0, count)`, not raw CRT
draws. Exactly one value in `[0, activeNormalArcCount)` must be supplied per
encountered conflict, in encounter order. Missing, negative, out-of-range or
unused choices return `InvalidConflictChoice`. Counts beyond the signed 32-bit
legacy range return `UnsupportedLegacyRange`. Other validation issues propagate
without returning a partial plan. Inputs remain unchanged on success and failure.

The named **legacy raw-arc-index policy** deliberately preserves the behavior
documented in [conflict characterization](conflict-characterization.md):

- Visit each eligible, unprocessed registered transition's inputs before outputs,
  using actual adjacency order from the [topology projection](conflict-topology.md).
- Count active normal arcs, including distinct parallel arcs to the same
  transition. Resource abundance does not suppress selection.
- Use the supplied choice as an index into the entire place adjacency array,
  including inactive, test and inhibitor arcs. Do not filter candidate indices.
- Clear internal visitation eligibility for every referenced transition and
  disable active transitions at other raw indices.
- Continue the current transition's remaining input/output loops after it loses
  a conflict. Recheck active/visitation state only on the next outer iteration.

Internal visitation state replaces the scheduling role of the legacy drawing
`Invalid()` flag. It is local to the call and is not exported as presentation
state. No alternate corrected policy or silent fallback is included. Inactive
raw indices can discard all candidates; parallel arcs can disable their sole
transition. Those remain explicit compatibility defects to address separately.

## Result and boundaries

`ConflictSelectionPlan` contains selected IDs and ordered `ConflictDecision`
records: branch/meet kind, visited transition, shared place, active normal arc
count, supplied raw index and transitions disabled in adjacency traversal order.
These records describe selection, not token animation or complete legacy logs.

The selector does not transfer tokens, update statistics/delays/time or validate
joint token effects. In particular, an abbreviated or empty supplied conflict
list can leave a batch that is unsafe to fire. Use `PlanLegacyTokenEffects` to
validate that batch before applying it; `PlanLegacyTiming` also performs that
validation. A full step composer and safe worker remain separate stages.

## Evidence and verification (2026-10-28)

Thirteen pure cases cover replay, immutability, branch/meet decisions and traces,
empty/explicit visitation order, blocked/waiting transitions, invalid inputs,
propagated validation, raw-index defects, parallel/mixed arcs, continuation after
losing and endpoint order independent of document order. New headers are checked
for accidental MFC/Windows imports.

The eight existing conflict-characterization cases now call `CheckSelection`.
It exports the combined snapshot before the real legacy step, copies the
fixture's ordered conflict list, and predicts winners with supplied known seeded
choices **before** executing `SingleStep`. It then compares the complete selected
ID vector. Winners are not copied from the kernel as prediction inputs. Existing
token and timing comparisons still run afterward.

Two dedicated differential cases add a three-step branch whose previous winner
becomes capacity-blocked, and independent branch/meet choices in both normal and
fast mode. These traces verify the MSVC seed/choice mapping used by the tests;
they do not validate a portable RNG or production pre-scan. No worker starts.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases (conflict comparisons extended) | 134/134 | 134/134 | 134/134 | 134/134 |
| Pure selection cases | 13/13 | 13/13 | 13/13 | 13/13 |
| Dedicated selection differential cases | 2/2 | 2/2 | 2/2 | 2/2 |
| CTest suites | 15/15 | 15/15 | 15/15 | 15/15 |

Win32 uses `build/vs2022-win32`, x64 uses `build/rules-x64`. The domain-only
Release library also builds in `build/rules-domain-only`. New domain/test targets
compile with `/W4 /WX /permissive-`; existing legacy warnings remain visible.
Production simulation and sample files are unchanged. GUI startup, printing,
editing, animation and background lifetime were not retested for an unused module.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(domain\.(conflict-selection|selection-differential)|simulation.conflicts)$' --output-on-failure
```

## Next step and rollback

The subsequent [pre-scan extraction](conflict-scan.md) verifies the structural
conflict list against the actual legacy loop without starting its worker.
Next, compose scan, eligibility, selection, token effects and timing into a single
synchronous step with explicit random inputs. Keep production activation, random
generator replacement, reset and background shutdown as separate work.

Rollback removes `ConflictSelection.*`, its domain source entry, the new
validation code, `ConflictSelectionTests.cpp` and the two CTest registrations.
Remove `CheckSelection` and its two dedicated cases, and restore the eight
characterization cases to direct `Step` calls. Topology and prior domain modules
can remain; no user document migration or UI rollback is needed.
