# MFC document/session lifecycle binding

`LegacyDocumentSessionBinding.*` connects an explicitly attached adapter to actual
`CPetriSimDoc` lifecycle and mutation entry points. It owns the value session;
the document stores only a weak observer. The production application does not
automatically attach this binding or replace its simulation commands yet.

## Ownership and invalidation

The legacy-compatible `HDocumentSessionLifecycle.h` defines a typed change reason
and observer interface without C++20 application headers. Registration permits
one live observer per document and checks the document's creating thread. The
binding also enforces owner-thread access for controls and queries.

`Attach(document, DocumentId)` returns an owned shared binding. `Start(seed)`
closes any previous session, validates a fresh offline export and creates a new
session identity. Export errors return diagnostics with the old session already
closed; they do not silently keep an obsolete run active.

Before a notified change, the binding records the first invalidating reason,
closes the session and joins its worker. Identity acceptance is disabled before
join by the session. If explicit closure throws, the mutation entry point does
not continue past the notification. A successful restart clears the old reason
and assigns a new session ID; delayed old command handles remain rejected.

The binding has a non-owning document pointer solely on the UI owner thread.
The document's destructor locks its weak observer, invalidates that pointer and
destroys/joins the remaining value session through a noexcept fallback. The
binding may then outlive the document safely; Snapshot becomes empty, old identity
is rejected, and a retained Completion future remains observable. Conversely,
binding destruction detaches its weak registration and joins its own service.
No worker receives a document or window pointer.

Explicit `Close()` is the error-reporting boundary. `Detach()` closes first and
removes registration; it cannot restart against a detached document. Normal
destructors only join. All callers must respect owner-thread lifetime rules.

## Notified entry points

| Entry point | Invalidation boundary |
| --- | --- |
| AddObjects / RemoveObjects | Before IDs, lists, maps or endpoints change; empty operations do nothing |
| DeleteContents | Before legacy shutdown, view notification and object deletion |
| OnCloseDocument | Before MFC base close/view/content cleanup |
| OnNewDocument / OnOpenDocument | Before base replacement processing |
| Serialize loading | Before reading/replacing archive contents; storing is unaffected |
| SetModifiedFlag(TRUE) | Conservative fallback, including repeated TRUE on an already dirty document |
| Document simulation commands | Before delegating to the original simulator |
| Selection tracking / property edits | Before point insertion, tracking or applying selected properties |
| Property preview and restoration | Before applying temporary/restored property values |
| Arrange and net-object tools | Before reordering or starting object/connector edits |

The modified-flag fallback alone would be too late for several edit flows, hence
the explicit tool barriers. Invalidation is deliberately conservative: geometry,
style, ordering or a cancelled edit can also close an attached session. Clearing
the dirty flag, saving and reads do not invalidate it.

Mutable DrawObjectList access and direct model setters remain legacy escape
hatches. Code using them outside these entry points must call BeforeNetworkChange
first. This stage does not claim automatic detection of arbitrary raw model
mutation, exhaustive interactive edit coverage, or a complete command model.
The old worker and its unsafe shutdown are unchanged.

## Tests and verification (2026-10-28)

Twelve MFC characterization cases execute real document APIs: pre-add/remove
observer order, duplicate/weak registration, already-dirty invalidation, no-op
preservation, add/remove, DeleteContents, OnCloseDocument, in-memory archive
save/load, multi-document isolation, both destruction orders, detach/wrong-thread
registration and legacy reset routing. Futures verify workers have joined when
invalidation returns; closed identities are rejected. A retained old reply is rejected
after re-export. No legacy simulation worker is started by these tests.

The initial test build exposed a protected OnSimCommand access in the test;
a using declaration in the test subtype fixed it without changing its production
visibility. The first Debug run then timed out on the legacy reset command's
missing status-window assertion in the new fixture. A scoped message-only window
fixed that fixture requirement; assertions and the 30-second timeout remain enabled.
Final Debug binding checks also pass three repeated runs per architecture.
PetriSimDoc.cpp and SelectTool.cpp retain their original single-byte
encoding; edits were checked for byte-preserving round trips and narrow diffs.
Existing no-final-newline files were preserved.

| Checks | Win32 Release | Win32 Debug | x64 Release | x64 Debug |
| --- | --- | --- | --- | --- |
| Application/libraries/test build | Passed | Passed | Passed | Passed |
| Previous console cases | 252/252 | 252/252 | 252/252 | 252/252 |
| Binding cases | 12/12 | 12/12 | 12/12 | 12/12 |
| Console CTest suites | 29/29 | 29/29 | 29/29 | 29/29 |
| Startup/new document + six sample GUI tests | 7/7 | Not repeated | Not repeated | Not repeated |

Win32 uses build/vs2022-win32; x64 uses build/rules-x64. Portable libraries also
build in build/rules-domain-only Release. Win32 Release GUI checks use the real
application, verify responsiveness, normal closure and unchanged reference hashes.
They do not exercise drag gestures, previews, printing, token animation or active
session rendering. Existing legacy warnings remain visible. Archive serialization
layout and sample files are unchanged; the new observer is not serialized.

```powershell
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release -R '^application\.session-binding$' --output-on-failure
```

## Next boundary and rollback

Add a controlled presentation bridge that reads tagged session values and rejects
stale identities before showing them in an opt-in UI flow. Keep model write-back,
simulation-command replacement and animation separate. Complete interactive edit
coverage before enabling a session automatically for every document.

Rollback removes the binding files/target entry and test file/CTest registration,
the weak lifecycle header/member/methods, and the narrow document/tool barriers.
Restore SetModifiedFlag inheritance and the original document destructor body.
Prior standalone sessions/services remain usable without archive conversion.
