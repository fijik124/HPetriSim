## HPetriSim

HPetriSim has a graphical editor which provides basic editing and simulation of Petri Nets. 
The tool is useful for beginners such as students in order to get familiar with Petri Nets.

#### The program supports Place/Transition Nets:
- Places with limited capacity
- different edge types with weights
- timed transitions 

#### The editor supports:
- graphic objects can be positioned, moved, and deleted
- text and geometrical objects available for annotations
- zoom function
- print function
      
#### Components:
- Graphical Editor
- Token Game Animation
- Fast Simulation
- Simple Performance Analysis

### Build with CMake (migration in progress)

The application still uses the existing MFC UI, simulation and `.hps` reader.
The CMake build is the first migration step. Win32 and x64 Debug/Release builds
were verified with Visual Studio 2022 Community 17.14.43, MSVC 19.44.35229,
Windows SDK 10.0.26100 and CMake 3.31.6 on 2026-10-28.

Startup no longer enumerates printer-driver fonts, which blocked initialization
on the test machine. The document header now supports both historical Win32 and
x64 markers and writes the original Win32-compatible version-2 header. Simulation
rules and object payload formats are unchanged. See
[regression findings and rollback](docs/startup-and-archive-regressions.md).

Requirements:

- Windows 10/11.
- Visual Studio 2022 or Build Tools 2022 with Desktop development with C++,
  MSVC v143 x86/x64 tools, C++ MFC for v143, and a Windows 10/11 SDK.
- CMake 3.25 or newer (the Visual Studio CMake component is sufficient).

Import `.vsconfig` in Visual Studio Installer to select the required components.
In Visual Studio 2022, use **File > Open > Folder** on this repository and select
the `vs2022-win32` CMake configure preset. Use the CMake target `HPetriSim` as the
startup target. The original `.sln` still has its historical toolchain settings.

From the repository root in a Developer PowerShell with CMake on PATH:

```powershell
cmake --preset vs2022-win32
cmake --build --preset win32-debug
cmake --build --preset win32-release
```

Executables are in `build/vs2022-win32/bin/Debug/HPetriSimD.exe` and
`build/vs2022-win32/bin/Release/HPetriSim.exe`. For x64:

```powershell
cmake --preset vs2022-x64
cmake --build --preset x64-debug
cmake --build --preset x64-release
```

Both architectures use static MFC and the matching static debug/release CRT.
The application keeps its compiler-default language mode. The new domain library
and legacy adapter use C++20; domain targets use strict conformance and warnings.
Source files are explicitly listed in `cmake/LegacySources.cmake`.

Builds currently emit legacy warnings, including unsafe `swscanf_s` argument
types in `IniSection.cpp` and narrowing conversions in x64. They remain visible
and have not been suppressed or repaired as part of the build migration.

After a successful build, open copies of all six documents in `sample/`, save
and reopen them, and check editing, step/run/fast/pause/reset, animation, zoom
and print preview. Do not overwrite the reference samples. Compilation alone
does not establish `.hps` or simulation compatibility.

### Automated checks

With `BUILD_TESTING=ON` (the default), CMake downloads Catch2 3.8.1 using a pinned
SHA-256. Its six archive-header regression cases run without opening the UI:

```powershell
ctest --test-dir build/vs2022-win32 -C Release -R persistence --output-on-failure
```

Enable the seven startup/load/shutdown tests in an interactive Windows session:

```powershell
cmake --preset vs2022-win32 -DHPETRISIM_UI_TESTS=ON
cmake --build --preset win32-release
ctest --test-dir build/vs2022-win32 -C Release --output-on-failure
```

The 23 simulation characterization cases execute the legacy step synchronously,
using actual places, transitions and arcs, without starting a worker thread:

```powershell
ctest --test-dir build/vs2022-win32 -C Release -R simulation --output-on-failure
```

They cover marking, capacities, all arc/time modes, limits, reset and seeded
conflicts. Known defects are explicitly tagged rather than silently fixed. See
[simulation behavior and test boundaries](docs/simulation-characterization.md).

Ten pure domain cases and ten adapter cases cover typed IDs, validation, independent
snapshots, field mapping and conversion of all six sample archives. The application
still uses its original model. See [domain boundaries and migration](docs/domain-model.md).

```powershell
ctest --test-dir build/vs2022-win32 -C Release -R domain --output-on-failure
```

For a domain-only build, configure with `-DHPETRISIM_BUILD_MFC_APP=OFF`. Adding
`-DBUILD_TESTING=OFF` builds just the library without downloading Catch2. This path
does not require MFC and does not build the Windows UI or adapter.

The pure `EvaluateLegacyEligibility` evaluator now covers pre-conflict arc and
delay conditions. Fifteen rule cases and seven differential cases compare it with
the real legacy step, including 201 parameter combinations and a two-step chain.
Candidates are distinguished from actual conflict winners. See
[rule semantics, limits and verification](docs/simulation-rules.md).

`PlanLegacyTokenEffects` now plans marking and statistics for explicitly selected
transitions. Fifteen pure cases and three dedicated differential cases cover it;
existing step tests also compare its results with the real legacy token-transfer
code. It is not activated in the UI. See [token effects and safety limits](docs/token-effects.md).

`PlanLegacyTiming` now plans transition delays, logical time, step count and stop
reasons from explicit stochastic samples. Eighteen pure cases and four dedicated
differential cases cover it; the fixture also compares timing on every legacy
step. Random generation and conflict selection remain outside this API. See
[timing behavior and verification](docs/timing.md).

Eight additional legacy conflict cases cover adjacency order, mixed arc kinds,
parallel arcs and conflict continuation. Reconnection proves that document arc
order alone cannot replay selection.
See [conflict characterization and extraction requirements](docs/conflict-characterization.md).

`ExportLegacySimulationSnapshot` now copies and validates actual endpoint order
alongside the net snapshot. Nine pure topology cases, six adapter cases and
extended checks on all six sample archives cover this boundary. The production
selector is unchanged. See [topology contract and verification](docs/conflict-topology.md).

`PlanLegacyConflictSelection` now predicts winners and decision traces from that
topology, an explicit conflict order and explicit random choices. Thirteen pure
cases, two new differential cases and the eight extended conflict cases verify
the legacy policy, including known defects. It is not activated in the UI. See
[selection contract and remaining boundaries](docs/conflict-selection.md).

`FindLegacyPotentialConflicts` now supplies the structural conflict list in net
order. Eight pure cases and four legacy comparison/boundary cases verify it,
including 54 arc-kind/resource combinations. The original scan loop was moved
unchanged to a private method so tests can call it without starting a worker.
See [pre-scan semantics and compatibility limits](docs/conflict-scan.md).

Use the x64 directory/preset or `-C Debug` for other configurations. UI suites
must run sequentially and never save reference documents. Tests check that all
six sample files remain unchanged. Save/load round trips, editing, font preview,
printing, animation and background simulation still require further characterization;
these checks do not establish release readiness.

For an offline build, configure with `-DBUILD_TESTING=OFF`, or point
`FETCHCONTENT_SOURCE_DIR_CATCH2` at an already extracted Catch2 3.8.1 source tree.

### Legacy Visual Studio solution

`HPetriSim.sln` and `HPetriSim.vcxproj` are retained unchanged as a rollback
path. They target VS 2017/v141 and Windows SDK 8.1; their x64 settings have known
differences from Win32 and are not a verified baseline. Build the CMake-generated
solution for the new configuration. Removing the CMake files restores the old
build entry point without changing application sources or document formats.

The historical `setup/HPetriSim Setup.vdproj` is not part of the CMake build and
contains obsolete input paths. Installer modernization follows build and
behavior verification.
