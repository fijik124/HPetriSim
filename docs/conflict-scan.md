# Structural potential-conflict scan

`FindLegacyPotentialConflicts` in `src/cpp/domain/ConflictScan.*` returns either
ordered transition IDs or validation issues. It consumes an immutable snapshot
and explicit endpoint topology, uses standard C++20 and does not change model
state, UI flags, statistics, randomness or timing. The application does not use
this pure function yet.

## Preserved rules

After validating the complete snapshot and topology, visit transitions in net
order. Include a transition when any input place has more than one outgoing arc,
or otherwise when any output place has more than one incoming arc. Each
transition appears once, even if it qualifies through multiple inputs/outputs.
Isolated transitions and simple chains do not qualify.

All arc kinds and distinct parallel arcs count. The scan ignores marking,
capacity availability, weights, current active/enabled flags and remaining
delays. It identifies structural possibilities, not actual firing conflicts.
Eligibility and ordered conflict resolution still occur later. It does not run
`EvaluateLegacyEligibility` or impose that evaluator's signed 32-bit arithmetic
subset; the scan itself performs no token/delay arithmetic.

An invalid snapshot or incomplete/corrupt topology returns diagnostics without
a partial list. No default order is inferred from IDs, map iteration or document
arc order. The result can supply the conflict-order input to
[legacy conflict selection](conflict-selection.md).

## Actual legacy test boundary

The existing pre-scan loop was moved verbatim from `CHSimulation::Online()` into
a private `ScanPotentialConflicts()` method in `HSimulation.cpp/.h`. `Online()`
calls it at the same point, after preparing the transition arrays and before
resetting active/enabled flags, logging and creating its worker. The moved loop
was compared against HEAD and matches exactly after newline normalization.
It has no dependency on the new domain module.

The private method preserves the legacy behavior of appending to the prepared
conflict list and setting `Enabled(true)` and `Invalid(true)` for qualifying
transitions. It does not clear the destination or reset active/enabled flags.
`Online()` retains those responsibilities. The friend test fixture explicitly
clears its conflict list before each scan and calls the real method without
creating events, a worker, a timer or a visible window.

This is a test seam, not a new public API or a thread-lifetime fix. It verifies
the actual scan loop, not the entire `Online()` initialization/lifecycle. In
particular, the test does not execute the subsequent flag resets or thread
creation.

## Disconnected-arc compatibility limit

The legacy scan skips a disconnected connector when visiting a transition's
input/output arcs, but counts all entries in a neighboring place's adjacency
array. A dangling arc attached only at its source can therefore mark a connected
peer as potentially conflicted. A connector attached only at its destination is
skipped and does not inflate that source's outgoing count.

A dedicated boundary test records both cases through the real scan and checks
that the combined export returns `DisconnectedArc`. It never executes a legacy
simulation step on these malformed graphs. The pure API deliberately requires
complete valid topology and does not attempt to reproduce dangling-pointer or
partial-connector simulation behavior. Editing incomplete arcs remains possible
in the legacy editor; activating a future pure simulator will need an explicit
diagnostic at that boundary.

## Tests and verification (2026-10-28)

Eight pure cases cover empty/isolated nets, chains, branches/meets of every arc
kind, state immutability, parallel arcs, multiple qualifying endpoints, net order
and propagated validation errors. The header is checked for accidental MFC or
Windows dependencies.

Four legacy comparison/boundary cases cover:

- 54 branch/meet combinations of both arc kinds and token levels (0, 2, 10),
  comparing pure predictions and explicit expected lists with the real scan.
- Multiple qualifications, isolated transitions, list order and repeated scans;
  checks also preserve the legacy scan's flag behavior.
- Both disconnected-endpoint boundaries described above.
- Reconnection in both directions and parallel arcs, feeding the actual scanned
  list into the already verified pure selection/legacy-step comparison.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases | 149/149 | 149/149 | 149/149 | 149/149 |
| Pure scan cases | 8/8 | 8/8 | 8/8 | 8/8 |
| Legacy scan comparison/boundary cases | 4/4 | 4/4 | 4/4 | 4/4 |
| CTest suites | 17/17 | 17/17 | 17/17 | 17/17 |

Win32 uses `build/vs2022-win32`, x64 uses `build/rules-x64`. The domain-only
Release library also builds in `build/rules-domain-only`. Existing legacy
warnings remain visible. Sample documents and serialization are unchanged.
GUI startup, printing, animation and background lifecycle were not retested;
the new domain scan is not activated in the application.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^domain\.(conflict-scan|scan-differential)$' --output-on-failure
```

## Next smallest step and rollback

The subsequent [complete-step composition](simulation-step.md) combines scan,
selection, token effects and timing with explicit random inputs, returning a
complete independent state only after all phases succeed. Repeated-step
comparisons use the real kernel. Engine state ownership, UI activation, a new
random generator, reset and safe background shutdown remain separate work.

Rollback removes `ConflictScan.*`, its domain source entry, `ConflictScanTests.cpp`,
the two CTest registrations, and scan helpers/four cases in the fixture. Inline
the unchanged legacy loop back into `Online()` and remove the private method
declaration. Existing selection/topology modules can remain. No archive or user
data migration is involved.
