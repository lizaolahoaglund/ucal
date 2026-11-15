# UCAL Manager user release package

Extract the Windows ZIP into a writable, versioned directory and start
`UcalManager.exe`. The package contains the Manager and the runtime components
needed for its supported integrations. Original vendor applications and hardware
drivers are separate installed prerequisites.

## Runtime layout

| Location | Content |
| --- | --- |
| Package root | Manager, matching Qt/MinGW runtime and license summary |
| `runtime/x64`, `runtime/x86` | Broker/workers, J2534 host/adapter, native backend catalog and required C++ runtime |
| `runtime/<architecture>/discovery` | Original UCAL J2534 discovery adapters |
| `backends/<id>` | Static backend manifest and original UCAL adapter DLLs |
| `frontends/<id>/<architecture>` | Frontend, shared support and bridge binaries; each runtime asset has one canonical location |
| `licenses` | Project permissions and selected dependency notices |
| `SOURCE.txt` | Readable source-download information: exact revision, companion filename and SHA-256 |

Developer PowerShell launch/deployment utilities, forwarder generators, test
harnesses, SDK headers, import libraries and build projects belong in the Git
repository. They are not included in the user ZIP. A required bridge EXE or support
DLL remains a runtime dependency even when its implementation is maintained privately.

The source archive named by `SOURCE.txt` is a separate attachment beside
the binary ZIP. Distribute both through the same release with equivalent source
access. The archive contains the exact covered public code and build instructions;
private compatibility implementations and internal Git history are excluded.
Dependency notices do not replace any dependency-source delivery obligations.

## Settings and profiles

The package's `backend.json` describes a module and stays unchanged during use.
Native runtime-path overrides belong in `data/backend-plugins/<id>.ini`, under
Manager settings. The optional setting is `[runtime] library=...`; absence or an
empty value selects the adapter's installed-runtime discovery. Manager supplies
the configuration path to the backend through its generated `catalog.ini`.
Application logical-channel mappings remain in their application profiles.

Manager preserves an existing user settings file. When an older module contains
custom settings, they can be imported into the Manager settings directory without
changing the module; relative runtime paths retain their original meaning. Empty
module templates do not create settings files. A custom settings-directory override
continues to isolate the complete Manager state.

Keep `data/` when replacing a portable program directory. It contains saved
preferences, profiles and generated shortcut icons. Desktop shortcuts reference the
Manager executable and their generated composite icon; update or recreate shortcuts
when moving the program directory. Icons use the installed application's artwork
plus the UCAL image embedded in Manager, so no vendor icon collection is shipped.

## External software prerequisites

Use independently installed, licensed vendor applications and hardware drivers.
The package contains no CANoe/PCAN-View executables, original vendor runtime DLLs,
Windows SDK headers or signing material. A compatibility DLL with an external API
filename is an original UCAL implementation, not a bundled vendor runtime.

For CANoe's application-local comparator, select the installed software, create
an application and start it from Manager. The first launch prepares the complete
local compatibility copy under the user's application-data directory from that
installation and the installed Vector runtime. This requires readable installed
files and enough disk space for the execution directories; no compiler or
developer scripts are needed. Preparation is cancellable and leaves the vendor
installation untouched. Subsequent launches reuse the copy and synchronize UCAL
modules while preserving saved settings. Its Provider profile remains a
separate candidate. PCAN-View retains the bounded image/version qualification in
its module documentation. Merely having a vendor application installed does not
establish that its selected compatibility profile is ready.

## Build and validate

To build the public components and run their regression tests, follow the
[Windows regression guide](../../test/Running.md). For a portable package,
also collect the dependency notices and the separately distributed compatibility
binaries. The Vector and PCAN-View package manifests list the filenames and
architectures. Place the required binaries under `out/x64/Release` or
`out/Win32/Release` to match the architecture. You can supply the directory for
the native PCAN-View adapter through `-PcanViewNativeRoot`.
See the [Vector manifest](../../frontends/vxlapi/plugin-package.json) and the
[PCAN-View manifest](../../frontends/pcanview/plugin-package.json).
Include both `UcalCanoeProvider.dll` and `UcalCanoeProvider64.dll` for the managed
CANoe launch copy. Manager uses these compatibility assets when preparing and
updating the copy for the RuntimeKernel/XL route. The separately named Provider
registration route requires separate qualification. Vector hardware support uses the
independent Vector XL backend.

Run these commands from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Build-UcalDist.ps1 `
  -QmakePath '<qt-kit>/bin/qmake.exe' -MakePath '<matching-toolchain>/bin/mingw32-make.exe' `
  -RuntimeNoticesRoot '<collected-runtime-notices>' -DistDirectory out/community -PublicOnly
```

When building from a source archive without Git metadata, also pass
`-SourceArchivePath` and its exact `-SourceRevision`. The package selects the
Microsoft C++ runtime DLLs required by the built binaries.

Generate that bundle with `tools/packaging/Collect-UcalRuntimeNotices.py`, passing
`--qt-source`, `--qt-kit`, `--mingw-licenses`, `--microsoft-notices` and a new
`--output` directory. The maintained profile covers Qt 5.14.2 MinGW 7.3 x64,
the shipped Windows plugins/ANGLE, embedded third-party components and the actual
GCC/MinGW/Microsoft runtime dependencies. A different kit needs a profile review.
The Microsoft input is a curated `Microsoft-Runtime.txt` containing the selected
C++ runtime terms and a notice identifying the redistributed Direct3D shader compiler.
Keep redistribution-rights evidence with the build inputs; do not ship the IDE
agreement, SDK developer agreement or saved web pages.

The collector produces one readable `ThirdPartyNotices.txt` and an external
`RuntimeNotices.json` build receipt. Assembly verifies the receipt's hash and
copies only the text into `licenses/`. Source-download information is readable
in `SOURCE.txt`. Raw attribution metadata and developer/toolchain documentation
are excluded by the user-package asset allowlist.

`tools/packaging/New-UcalManagerPackageManifest.ps1` checks required assets, DLL
architecture/dependencies, metadata and prohibited payload before recording every
file's relative path, length and SHA-256 in package schema version 2. The package
validator verifies that inventory and performs a bounded Manager/Broker smoke.
Its endpoint probe is a source-built test tool selected by `-ProbePath`, outside
the user package. `Test-UcalManagerPackageContract.ps1` exercises positive and
negative packaging cases, including missing support DLLs, SDK headers and vendor
application filenames.

After validation, create the portable ZIP from the assembled directory.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/packaging/New-UcalManagerPortableZip.ps1 `
  -PackageRoot out/community -OutputPath out/ucal-windows-x64.zip
```

Publish the tested ZIP and its matching companion source archive together. Building
or validating local artifacts does not publish a release or upload vendor software.
