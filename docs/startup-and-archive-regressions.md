# Startup and archive regressions (2026-10-28)

## Startup waits for a printer driver

Before the fix, the `startup.TokenGame` test failed after 20 seconds. CDB with
the application's Debug symbols showed the main thread inside this call chain:

```text
CPetriSimApp::InitInstance
CMainFrame::OnCreate
CMainFrame::InitializeShapeFormatContextRibbon
CMFCRibbonFontComboBox::RebuildFonts
CMFCToolBarFontComboBox::RebuildFonts
CWinApp::GetPrinterDeviceDefaults
CPrintDialog::GetDefaults
PrintDlgW / DocumentPropertiesW / printer RPC wait
```

The installed MFC source confirms that its font combo enumerates printer fonts
unconditionally. Changing its font-type mask alone would not avoid the call.

The ribbon now uses an editable combo populated from GDI+'s installed font
families, the same font source used to draw document text. Font names still flow
through the existing property and preview commands, including names typed by
the user or loaded from a document. Printer-only font variants are no longer
enumerated during startup. Print commands and printer selection are unchanged;
an unresponsive driver can still affect an explicit print operation.

## Win32 archives fail to open in x64

After startup was repaired, all six sample-opening tests failed in x64 Release,
while a new document worked. Inspection of MFC's `arccore.cpp` explains why:

- Win32 `WriteCount(-1)` writes six `FF` bytes.
- x64 `WriteCount(-1)` writes fourteen `FF` bytes.
- x64 `ReadCount()` treats the six-byte prefix as an instruction to read eight
  more bytes, consuming the version and part of the object list in Win32 files.

`HpsArchiveHeader.h` now reads the document header with explicit WORD/DWORD
fields. Both historical marker lengths are accepted. New writes retain the
original eight-byte Win32 header (six `FF` bytes followed by version `02 00`).
Object serialization and simulation fields retain their existing format.
Unknown document versions and malformed extended markers are rejected.

Catch2 tests cover both markers, exact writer bytes, unversioned object counts,
malformed markers and truncated data. This is not a complete independent `.hps`
reader or proof of compatibility with every historical object schema.

## Reproduce the checks

```powershell
cmake --preset vs2022-win32 -DHPETRISIM_UI_TESTS=ON
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release --output-on-failure
```

Repeat with the x64 preset/directory and with Debug. Catch2 is pinned by version
and SHA-256 and is downloaded during the first configuration with tests enabled.
`-DBUILD_TESTING=OFF` builds the application without downloading test dependencies.

The UI tests require an interactive Windows desktop. They start hidden instances,
wait for a responsive document window, request normal shutdown and verify the
input file's SHA-256 is unchanged. A zero process exit code alone is insufficient:
MFC can exit with zero after failing to open a document. Tests do not save files
or force-terminate a stalled application; a failed run may need inspection of its
reported process ID. Do not run multiple UI suites concurrently.

The GUI tests cover startup, sample loading and shutdown, not editing, printing,
save/load round trips, token animation or simulation correctness. Those need
separate characterization tests before their implementations change.

## Verification results

| Architecture/configuration | Application build | Startup tests | Archive-header cases |
| --- | --- | --- | --- |
| Win32 Debug | Passed | 7/7 | Not built in Debug |
| Win32 Release | Passed | 7/7 | 6/6 |
| x64 Debug | Passed | 7/7 | Not built in Debug |
| x64 Release | Passed | 7/7 | 6/6 |

The x64 verification build used `build/verify-x64` because a separately running
instance held `build/vs2022-x64/bin/Release/HPetriSim.exe` open. That process was
not closed or replaced. Do not mistake the locked, older binary for the tested
one in `build/verify-x64/bin/Release/HPetriSim.exe`.

## Rollback

The fixes are confined to `MainFrm.cpp`, the document's header read/write calls,
and `HpsArchiveHeader.h`. Reverting either fix independently restores that legacy
behavior. Keep the regression tests as evidence of the resulting failures.
No sample files were migrated or overwritten.
