# Domain model and legacy projection

This stage adds a C++20 value model and a one-way adapter. The MFC editor and
legacy simulator still own the live document. The adapter is built alongside
the application but is not called by UI commands or used to replace persistence.

## Boundaries and data

`hps_domain` uses only the C++ standard library. It has no PCH, MFC inheritance,
Windows/GDI+ types, archive reader, thread, drawing state or UI identifiers.
Both its generated project and its test project have MFC disabled. A separate
configuration with `HPETRISIM_BUILD_MFC_APP=OFF` successfully built the library.
This was verified with MSVC on Windows; Linux/macOS compilation was not tested.

| Type | Purpose |
| --- | --- |
| `PlaceId`, `TransitionId`, `ArcId` | Separate types; no implicit conversion from integers or between kinds |
| `NodeId` | Variant of place/transition ID; no untyped pointer endpoints |
| `Place` | ID, UTF-16 name, capacity and initial marking |
| `Transition` | ID, name, timing mode and initial/range configuration |
| `Arc` | ID, name, typed endpoints, kind and weight |
| `PetriNet` | Ordered definitions, preserving legacy iteration order |
| `Marking` | Current place tokens and produced-token statistics keyed by place ID |
| `TransitionState` | Remaining delay, consumed-token statistics and active/enabled flags |
| `NetSnapshot` | Independent copies of definitions and runtime values |

Snapshots are plain values, not an immutable repository or command model. Call
`Validate` after constructing or changing one. The exporter returns a snapshot
only when validation succeeds; otherwise it returns structured issues with
codes, object IDs and messages. It never returns partial success or repairs data.

Validation covers nonzero/duplicate typed IDs, positive capacities/weights,
initial/current capacity limits, endpoint existence, place/transition alternation,
known enums, timing configuration and missing/extra runtime state. Empty nets are
valid definitions; simulation can subsequently report their deadlock. Domain ID
namespaces are independent, while the legacy exporter additionally enforces the
legacy document's shared numeric ID space.

## Legacy adapter

`ExportLegacyNet(CPetriSimDoc&)` lives in `src/cpp/adapters/`. It must run on the
document-owning thread, with simulation offline. It explicitly refuses online
state, including online pause. This guard is not a lock for simultaneous editor
mutation; thread-safe snapshot publication remains a later simulation task.

The legacy API has non-const getters, so the parameter is not const even though
export does not modify the document. A read-only `CHPosition::InitialTokens()`
getter avoids routing initial marking through ribbon property conversions or
serializing an object merely to inspect it.

Names are copied as UTF-16 code units without pointer reinterpretation. Token
fields are really `ULONG32` even where old getters return signed `long`; the
adapter recovers those unsigned bits into 64-bit counts. Remaining delay stays
signed, preserving the observed negative countdown overshoot. This preserves
data, not a promise that the legacy engine handles values above `LONG_MAX`
correctly. A future engine needs explicit arithmetic and compatibility policy.

Drawing shapes and labels remain in the original document, outside this domain
projection. Connected arcs are mapped by object identity and checked against
stored IDs. Disconnected arcs, foreign endpoints, stale references, duplicate
legacy IDs and unknown modes are diagnosed. Draft documents can still contain
incomplete arcs in the editor; export refusal does not restrict editing or saving.
All six existing sample documents projected successfully without dropping nodes
or arcs. This is not a replacement document format and does not preserve layout
inside the domain snapshot; original persistence still preserves that layout.

No firing rules, conflict selection, random generator or reset semantics were
introduced into the new model. The characterized defects remain unchanged in
the running legacy application.

## Build and tests

```powershell
cmake --preset vs2022-win32
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^(domain\.|simulation\.|persistence\.)' --output-on-failure
```

To build only the domain library without MFC or test downloads:

```powershell
cmake -S . -B build/domain-only -DHPETRISIM_BUILD_MFC_APP=OFF -DBUILD_TESTING=OFF
cmake --build build/domain-only --config Release
```

With tests enabled in that configuration, only the pure domain tests are added.
Catch2's pinned dependency remains necessary for those tests. The normal Windows
configuration also builds `hps_legacy_adapter` and the legacy characterization
harness. The domain and adapter require C++20; linking the adapter into that test
harness additionally exercises legacy sources in C++20. The actual MFC application
target retains its original compiler-default language mode for this stage.

| Checks (2026-10-28) | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application, domain, adapter and test build | Passed | Passed | Passed | Passed |
| Domain model cases | 10/10 | 10/10 | 10/10 | 10/10 |
| Adapter cases, including six sample archives | 10/10 | 10/10 | 10/10 | 10/10 |
| Existing simulation cases | 23/23 | 23/23 | 23/23 | 23/23 |
| Existing archive-header cases | 6/6 | 6/6 | 6/6 | 6/6 |

The x64 configuration remains in `build/verify-x64`. Startup/GUI checks were not
repeated: the UI and simulation still use the original code; the only new legacy
model method is an unused inline getter. The first adapter-test build needed a
missing `<array>` include; the corrected build passed. Existing legacy warnings
remain visible. New domain targets compile with `/W4 /WX /permissive-` on MSVC.

## Rollback and next boundary

Remove the domain/adapter targets and their test entries/files, the new initial
token getter and the adapter-only guard test in the characterization fixture.
The original application still works independently; no document migration or
reverse conversion is needed. Existing uncommitted work is retained.

The subsequent [eligibility extraction](simulation-rules.md) adds a pure rule
evaluator over validated snapshots and compares it with the real legacy step.
It is not enabled in the application. Background execution,
snapshot publication, persistence migration, command history and UI replacement
are outside this stage.

The subsequent [conflict topology projection](conflict-topology.md) supplements
snapshots with validated per-node connector order for future conflict replay.
It preserves the existing net export API and leaves production selection unchanged.
