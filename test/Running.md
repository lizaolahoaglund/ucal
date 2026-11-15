# Reproducible Windows regression

Run from a source checkout with Visual Studio C++ Build Tools, a Windows SDK and
the appropriate Qt 5.14.2 MSVC kit. Build output, vendor programs, hardware and
developer configuration are not prerequisites. MSBuild uses the installed Visual
Studio toolset and Windows SDK; a qualification build can pin them with MSBuild
`/p:PlatformToolset=... /p:WindowsTargetPlatformVersion=...`.

## Manager only

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File manager/test/Test-UcalManager.ps1 `
  -QmakePath (Get-Command qmake.exe).Source -Architecture x64
```

The fixed entry point builds all original fixtures, frontend DLLs, the product and
the QtTest program, then checks the [functional case catalog](../manager/test/cases.json).
Repeat with the x86 Qt kit and `-Architecture x86`. `UCAL_QMAKE` can supply the qmake
path; `UCAL_VCVARSALL` or `-VcVarsAllPath` can select a custom compiler setup script.
There is no dependency on a local portable release package.

Use `-OutputDirectory` to select a new/empty evidence directory. Defaults are unique
directories under `out/regression/`. The [suite guide](../manager/test/README.md)
defines prerequisites, isolation, counting and skip policy. This is a Windows Qt
widget/process test: the Windows Qt platform plugin and a session able to create
windows are required. No operator must click the test windows.

## Complete hardware-independent gate

From the public source checkout, run the umbrella script with the matching Qt kits:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/Test-UcalDeterministic.ps1 `
  -QmakePath '<x64-qt-kit>/bin/qmake.exe' -QmakeX86Path '<x86-qt-kit>/bin/qmake.exe'
```

This builds the native core, public backends and API frontends for Debug and
Release on x86 and x64, and builds and runs the Manager Qt tests for each supplied
kit. The summary lists the architectures exercised. The PCAN-View support ABI uses
an original fixture. Installed vendor integrations require their own opt-in tests
and, where applicable, the matching separately distributed support binaries.

After the native build, run the CANoe traffic assertion fixture against the public
simulated backend:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File frontends/canoe/test/Test-UcalCanoeTrafficAssertions.ps1 `
  -EvidenceDirectory out/canoe-traffic-fixture
```

CI can split the public core and Qt suites into jobs using `-SkipCore` or
`-SkipQt`. Collect results from both jobs for the complete public gate.
The umbrella runner writes a summary plus per-stage logs under a new `out/ci/` run
directory, and propagates a failed stage as a nonzero exit code.

Publish `qt-*/junit.xml`, `qt-*/results.xml`, `qt-*/summary.json` and the stage logs
as CI artifacts. `results.xml` is the original Qt report; `junit.xml` retains warnings
as output and reports real failures and skips separately. The maintained
`Test-UcalQtReport.ps1` checks nine report-contract assertions and runs with every
Manager gate.

The default runners isolate `UCAL_*` state and never select installed vendor hardware
runtimes. Station-dependent scripts under [acceptance](../ucal/test/hardware/README.md) remain
opt-in and retain their own topology, DLL and installed-application prerequisites.

## Installed application lifecycle

The Windows lifecycle tests create real dialog and process fixtures without hardware
or installed vendor applications. Run them with Python, `pywin32` and `psutil`:

```powershell
python -B test/installed/test_lifecycle.py -v
```

The [CANoe installed runner](../frontends/canoe/test/software/README.md) adds
physical tests on a dedicated bus. The `--create-application` option starts with
an empty application list and creates and launches the application in Manager.
A pass requires the selected traffic checks, normal application exit and clean
resource release. Unexpected dialogs, early exits, timeouts and forced termination
fail the run. Keep the `result.json` and `lifecycle.jsonl` from every attempt.

The [CANoe versioned runner](../frontends/canoe/test/software/README.md#canoe-11-and-12-with-a-physical-pcan-backend)
runs the physical subsets for the installed CANoe versions and records each
missing version as skipped in the overall report.

## Source and plugin isolation

```powershell
python test/Test-UcalSourceBoundaries.py
python test/Test-UcalSourceExport.py
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/Test-UcalIsolation.ps1 `
  -QmakePath (Get-Command qmake.exe).Source
```

The source checker verifies framework dependencies, fixed endpoint identities and the
[Manager functional case catalog](../manager/test/cases.json). `Test-UcalLayout.py`
also checks plugin-owned metadata, independent hardware headers and public links.
The isolation runner copies committed source into clean paths containing spaces. It discovers the current source
groups in the checkout and reports each executed case.
Each native/J2534 source group is removed independently; the suite also builds UCAL
alone, core/Manager without plugins, and externally copied frontend/backend groups.
The UCAL-only case compiles and runs the public C-header layout check on x86/x64.
Standalone native frontends/backends build on x86/x64 outside the repository layout
against the copied neutral framework. Standalone software frontend DLLs and the
other product-isolation cases build Release x64; they do not imply x86 execution coverage.

Commit tracked changes before this source-copy proof. No previous build, vendor
runtime, device or personal configuration is copied. `-Case` selects a subset;
`summary.json` reports only completed cases, and an incomplete run fails.
The deterministic gate separately runs the complete x86/x64 behavior matrix.

`Test-UcalTools.ps1` builds an original PE export fixture on both architectures
and verifies named/ordinal forwarding plus rejection of a missing required export.
It also runs inside the UCAL-only isolation case. No vendor DLL is required.

Public source archives omit private source groups and internal records. Export from
a committed revision with Python 3.12 or newer:

```powershell
python tools/packaging/Export-UcalSource.py --output "out/public source" --archive-output out/public-source.tar
```

The exporter honors tracked `export-ignore` rules, rejects private descriptors and
internal records, and includes no Git history. The optional archive uses the same
validated files as the exported tree, excluding empty private module containers.
The public regression entry points run from the exported tree. Source export is separate from publishing a repository.

`Test-UcalNaming.py` audits original C/C++ source names and public C exports. Standard-library, Qt, Win32 and explicit legacy test exports are listed separately. `Test-UcalSourceBoundaries.py` includes this audit. The standalone consumer-side case includes software frontends and the API-only compatibility component; it does not claim a software plugin exists for every compatibility DLL.

Source export and runtime notice collection have hardware-free contract suites:

```powershell
python -m unittest discover -s tools/packaging -p 'test_*.py' -v
```

They verify reproducible source archives, private directory exclusion, preserved
existing outputs, retained copyright/license text, required embedded components,
kit feature changes and exclusion of unrelated platform/developer material. The
package contract suite also rejects raw attribution JSON in the user ZIP.

## Independent API clients

### Tests from public source

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/Run-Tests.ps1
```

In a public checkout, this builds the VXLAPI DLL, its discovery sidecar and the
static-import test client for x86 and x64, and runs the shared Vector channel
catalog tests. It also checks PCAN-Basic traffic with simulated PCAN and Vector
backends and runs the PCAN-Basic deployment and restore tests.

### Full API regression with Vector support binaries

After the public build, place the matching separately distributed Vector support
DLLs beside the VXLAPI build outputs:

| Test client | Required file in the source checkout |
| --- | --- |
| x86 | `out/Win32/Release/UcalVectorSupport.dll` |
| x64 | `out/x64/Release/UcalVectorSupport64.dll` |

Then run the combined VXLAPI and PCAN-Basic regression:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File test/Test-UcalApiFrontends.ps1 -SkipBuild
```

The runner deploys matching DLLs beside each static-import test client. An
isolated Broker exercises simulated PCAN, Vector and J2534 runtimes without
vendor applications or physical hardware. The tests check Classic traffic,
supported FD/BRS traffic, the loaded DLL paths, clean shutdown, unchanged
executables and restore. The [API case catalog](cases.json) also covers the
expected rejection of explicit FD sample points on the generic J2534 route.
