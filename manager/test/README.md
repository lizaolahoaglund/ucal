# UCAL Manager regression tests

The [case catalog](cases.json) lists functional QtTest cases by stable ID.
The runner reports the functional count per architecture in `summary.json`.
QtTest's own totals also include `initTestCase` and `cleanupTestCase`.
Both architecture kits run the same catalog.

## Run from a clean checkout

Use Windows, Windows PowerShell 5.1 or newer, Visual Studio C++ Build Tools with a
Windows SDK, and the **Qt 5.14.2 MSVC kit matching the requested architecture**.
No vendor SDK, CAN hardware, CANoe, PCAN-View, Python, prepared package or previous
build output is required for this suite.

Put the selected kit's `qmake.exe` on this process's PATH, or pass its full path:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File manager/test/Test-UcalManager.ps1 `
  -QmakePath (Get-Command qmake.exe).Source -Architecture x64
```

For x86, select the x86 kit and pass `-Architecture x86`. The runner discovers
Visual Studio using `vswhere` and initializes its compiler environment. A custom
installation can supply `-VcVarsAllPath`; neither script nor source requires a
particular drive letter or Visual Studio edition. It does not change the system PATH.

Each run creates a fresh output directory under `out/regression/`. It builds the
original frontend ABI fixture, the two frontend DLL packages, the product executable
and the QtTest executable directly from tracked source. Fixture DLLs and the product
path are configured automatically. The product is needed for the genuine shortcut
forwarding test, even though no vendor application is launched.

This Windows widget/process suite uses the `windows` Qt platform plugin. Run CI in
a Windows session that can create windows; no manual clicking is required. Qt 5.14.2
`offscreen` does not implement the native dialog/window contract exercised here.
An unavailable system tray skips only `trayCloseHidesAndOpenRestores`, with an explicit
reason in every report. Other unexpected skips fail the gate. Linux/macOS execution
of this Windows-specific suite is not currently a supported regression target.

## Results and case maintenance

| File | Purpose |
|---|---|
| `functions.txt` | Function list discovered by the built QtTest executable |
| `results.txt` | Original QtTest text results |
| `results.xml` | Original QtTest XML, including warnings and skip reasons |
| `junit.xml` | CI-compatible results with pass/failure/skip distinguished |
| `summary.json` | Architecture, case count, per-case status, skipped IDs and process exit code |
| `report-contract/` | Nine assertions checking result decoding and JUnit generation |
| Per-project build logs | qmake/compiler output for all fixtures and executables |

The runner checks that `cases.json`, Qt's discovered functions and the final report
contain the same case IDs. Missing, renamed, duplicated, unexecuted or undeclared cases
cannot silently yield a successful full-suite result. A crash, timeout, malformed XML,
failed assertion or unexpected skip fails the run. Ordinary Qt warnings remain in
the logs without being misclassified as test failures.

When adding a regression, add a QtTest slot and its assertions to
[ManagerModelTest.cpp](ManagerModelTest.cpp), then update `cases.json` in the same
change. Reuse QTemporaryDir, the original fixture DLL and Qt's timer/signal/widget
helpers. Use application APIs and widget interactions rather than screen coordinates.
Tests must not depend on a developer's saved applications, installed vendor software,
serial numbers or private build directories. Run both architecture kits before merging.

The test executable also creates its own temporary settings/backend context when
launched directly, so a Qt Creator run cannot overwrite the user's portable settings
or attach to the product Broker. The supported runner supplies the required fixture
paths; a direct run without them gives an explicit setup failure.

## Scope

Cases cover mapping, persistence/recovery, backend/frontend package contracts,
software discovery, projects, launch planning, genuine shortcuts and instance IPC,
model/widget workflows, native Windows notifications, settings, language, tray and
process watchdogs. The [test guide](../../doc/baseline/tests.md)
maps behavior to named cases.

Physical USB changes and installed vendor applications are separate, opt-in
acceptance gates. Their absence is not a failure of this hardware-free suite and
their results are never fabricated by fixture success. See the
[USB procedure](../../ucal/test/hardware/README.md) and
[existing acceptance runners](../../ucal/test/hardware/README.md).
