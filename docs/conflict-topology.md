# Validated conflict topology projection

`src/cpp/domain/ConflictTopology.*` adds a standard C++20 value model alongside
`NetSnapshot`. `ConflictTopology` maps typed `NodeId` keys to `NodeAdjacency`,
whose incoming/outgoing vectors contain ordered `ArcId` values. Every node has
an entry, including isolated nodes. The vectors preserve actual endpoint order;
map key order does not define transition visitation order. That remains in
`PetriNet::transitions`. The existing snapshot and archive layout are unchanged.

This addresses the [characterized reconnection behavior](conflict-characterization.md):
document object order alone cannot reconstruct the conflict pass's connector
order. Different endpoints may have different relative orders even for parallel
arcs. The topology is explicit; it is not derived by sorting arc IDs or filtering
the document arc list.

## Validation contract

`ValidateConflictTopology(snapshot, topology)` first validates the snapshot and
returns its diagnostics if IDs/endpoints/state are invalid. For a valid snapshot
it checks:

- Every known place/transition has adjacency; unknown node keys are rejected.
- Every listed arc exists and agrees with the node and incoming/outgoing direction.
- An arc ID is not repeated within an endpoint list.
- Every arc is present at its source's outgoing and destination's incoming list.

Distinct parallel arcs are valid. Typed place/transition IDs may share numeric
values in the pure model. Validating does not repair lists, reorder input or
require that the chosen ordering be derivable from a particular editing history.
Diagnostics use `MissingAdjacency`, `UnexpectedAdjacency`, `InvalidArcReference`,
`DuplicateArcReference` and `MissingArcReference` with object IDs and messages.

## Legacy adapter boundary

`ExportLegacySimulationSnapshot` in `src/cpp/adapters/LegacyNetAdapter.*` returns
either `LegacySimulationSnapshot { snapshot, topology }` or validation issues.
It first uses the existing `ExportLegacyNet` validation, registers document
connector pointers, then copies each actual `ConnectorsIn/Out` list. Pointer
membership is checked before accessing an adjacency entry's object; null or
unregistered entries return `StaleReference`. The pure validator checks the
completed projection. Any failure returns diagnostics without a partial export.

The result owns its values and retains no MFC object pointers. Later document
changes do not alter either snapshot or adjacency. The document itself is not
modified. Existing `ExportLegacyNet` callers and behavior are unchanged; callers
needing conflict replay must request the combined export explicitly.

Both exports require the document-owning thread and an offline simulation.
Online state returns `SnapshotUnavailable`, including for the combined export.
These functions do not synchronize concurrent legacy mutation and do not make
arbitrary invalid document object pointers safe. The new membership check covers
adjacency entries against a valid document registry, not all possible memory
corruption.

## Tests and verification (2026-10-28)

Nine pure cases cover typed IDs, empty/isolated nets, missing/unknown nodes,
missing/unknown/duplicate arc entries, direction/endpoint errors, independent
orders, mixed parallel arcs and invalid snapshot diagnostics. The header test
rejects accidental inclusion of MFC/Windows headers.

Six dedicated adapter cases cover reconnection in both directions, independent
copies, isolated nodes, mixed parallel arcs, corrupted lists, null/unregistered
references, propagated validation errors and empty/drawing-only documents.
Corruption tests restore borrowed adjacency with an RAII guard before fixture
destruction. Existing online-guard coverage checks both exports. Existing sample
tests now compare every node's exported adjacency against its actual arrays in
all six `.hps` samples, opened read-only.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application and test build | Passed | Passed | Passed | Passed |
| Previous cases (sample/online coverage extended) | 119/119 | 119/119 | 119/119 | 119/119 |
| Pure topology cases | 9/9 | 9/9 | 9/9 | 9/9 |
| Dedicated topology adapter cases | 6/6 | 6/6 | 6/6 | 6/6 |
| CTest suites | 13/13 | 13/13 | 13/13 | 13/13 |

Win32 uses `build/vs2022-win32`; x64 uses `build/rules-x64`. The domain-only
Release library also builds in `build/rules-domain-only` without MFC. Existing
legacy compiler warnings remain visible. Production simulation and sample files
are unchanged. GUI startup, printing, editing and animation were not retested for
an adapter that is not activated in the UI.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^domain\.(topology|topology-adapter|legacy-adapter)$' --output-on-failure
```

## Next step and rollback

The subsequent [selection extraction](conflict-selection.md) uses this projection
with explicit conflict order, random choices and diagnostic traces. Known
raw-index and parallel-arc behavior remains explicit compatibility policy.
The production pre-scan, complete step composition and worker lifetime remain
separate work; this topology stage itself does not choose transitions or replace
unsafe shutdown.

Rollback removes `ConflictTopology.*`, its domain CMake entry/validation codes,
the combined export declarations/implementation, topology test files/CTest
entries, and added sample/online assertions. Keep the prior conflict
characterization cases and existing net export. No user data migration is needed.
