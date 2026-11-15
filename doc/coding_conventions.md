# Ucal coding and integration conventions

Ucal is the code identifier for Unified CAN Abstraction Layer (UCAL). The graphical product is **UCAL Manager** and its executable is **UcalManager.exe**. UCAL remains the acronym in protocol/documentation and the prefix of existing macros and environment variables.

| Surface | Convention | Examples |
| --- | --- | --- |
| Code namespaces | PascalCase | `Ucal`, `Ucal::Manager`, `Ucal::Backend` |
| Classes, structures, type aliases and C++ enum values | PascalCase | `BackendInterface`, `CanFrame`, `Status::InvalidArgument` |
| Original public C exports | PascalCase with Ucal prefix | `UcalBackendGetApi`, `UcalFrontendGetApi`, `UcalJ2534DiscoverV2` |
| C function-pointer types | PascalCase, Function + version suffix | `UcalBackendGetApiFunctionV1` |
| C++ methods, free helpers, ordinary fields and C record fields | lowerCamelCase | `openDevice`, `readCanEvents`, `abiVersion`, `ownerPid` |
| Private C++ members | lowerCamelCase; an existing trailing underscore is retained | `pipeName_`, `workspacePath_` |
| C/C++ module and public-header files | PascalCase | `MainWindow.cpp`, `BackendPlugin.h`, `ForwarderMock.c`; `main.cpp` is an entry-point exception |
| Original binary artifacts | PascalCase | `Ucal.lib`, `UcalBroker.exe`, `UcalBackendJ2534.dll` |
| Source folders | lowercase / lower_snake_case | `ucal`, `frontends/pcanview`, `backends/pcan_basic` |
| Macros, environment variables and C enum constants | UPPER_SNAKE_CASE | `UCAL_BACKEND_ABI_V1`, `UCAL_MANAGER_SETTINGS_DIRECTORY` |
| PowerShell / Python | Native language conventions | `Verb-Noun.ps1`; Python snake_case functions |

## Software and API ownership

`frontends` contains independent consumer API frontends (`vxlapi`, `pcan_basic`, and optional private groups) and software profiles (`canoe`, `pcanview`). API groups own their declarations, implementation and deployment metadata. Software profiles own discovery/launch and application-specific adapters. The Vector family's loading shells share one data plane under `vxlapi`; API acceptance remains distinct from installed-application acceptance.

API-only frontend groups also live under `frontends`; they implement an external API without automatically providing an installed-software discovery/launch template. Hardware drivers belong in `backends`; the neutral library and host live under `ucal`; Qt control-plane code lives under `manager/src`.

A concrete component folder owns its source, build files, static JSON and resources. The neutral solution/Manager imports no concrete plugin header. Build/package discovery skips an absent source group. Module tests live in their own `test`; cross-module tests live in the root `test`; generated runtime files live under `dist`; ignored build/evidence files live under `out`.

## Required external spellings and stable data

Standard-library, Qt and platform contracts keep their required names: `std::vector<T>::push_back`, `std::uint32_t`, `static_assert`, `qobject_cast`, `IMAGE_DOS_HEADER::e_lfanew` and `DllMain`. Vendor ABI spellings remain exact for both calls and compatibility exports, including `BM_*`, `CAN_*`, `xl*`, `PassThru*` and their required DLL filenames. Our own implementation aliases use Ucal PascalCase. Reusable audits report these standard exceptions separately.

JSON keys, command-line flags, package IDs, environment dictionaries, saved-state filenames, wire tags, numeric operation IDs and endpoint-identity bytes are versioned data rather than C++ names. They retain their existing schemas. Frontend ABI V1 JSON remains camelCase; V2 remains snake_case. Application data remains schema 3, backend metadata schema 5 and frontend/source metadata schema 2. Configuration/resource/document filenames follow their existing format conventions.

Original C exports use Ucal PascalCase. The loader also accepts `ucal_backend_get_api` or `ucal_frontend_get_api` from compatible plugins after validating the versioned function table.

DLL boundaries use C or explicitly serialized bytes, never STL/Qt layouts or C++ exceptions. Add only implemented contracts and use version/size negotiation for incompatible extensions. See [C contracts](../ucal/doc/README.md), [package contracts](../manager/doc/plugin-packages.md), [requirements](baseline/requirements.md) and [tests](baseline/tests.md).

## Checks

```powershell
python test/Test-UcalNaming.py
python test/Test-UcalSourceBoundaries.py
```

The source-boundary entry point includes the naming audit, fixed identity vectors and the Manager case catalog. Runtime regression and installed-product checks are documented in [CI](../test/Running.md).

Public display text uses **UCAL Manager**. Types and original binary names use `UcalManager`; a lowerCamelCase function prefix is `ucalManager`. Persisted application identifiers and registry value names are data contracts and retain their existing spelling. See [file placement and source roles](layout.md).

## Directory roles

Test-source directories use singular `test` at every level. Collection directories use plural names: `tools`, `frontends`, `backends`, `scripts`, `plugins` and `mocks`. Established abbreviations remain `src`, `doc` and `res`; component names such as `ucal`, `manager`, `core` and `ipc` remain singular.

Executable test-driver source belongs in `runner`; simulated libraries belong in `mocks`, and fixed test inputs belong in `res`. Directory roles do not rename existing executable artifacts, command-line options or environment-variable contracts.

When an API is implemented on both sides, use the same component folder name under `frontends` and `backends`, such as `vxlapi`, `pcan_basic`. The parent directory defines its direction: a frontend translates consumer calls into UCAL; a backend translates UCAL operations into the hardware API. Source folder names do not replace versioned runtime identities or saved configuration keys.

`res` contains static inputs/assets. Native declarations, mock DLL source and executable helper scripts stay with their implementation. Test data is under `ucal/test/res`, mock libraries under their owning component's `mocks`, Manager icons under `manager/res`, and runtime helper source scripts under the owning frontend's `scripts`.

Keep the contract spelling of test DLL output names, test environment keys, C export
vectors and runtime package destinations. Test DLLs may retain Fixture in their output
names. Preserve existing serialized identities and functional case IDs. See the
[Manager case catalog](../manager/test/cases.json) for the current cases.
