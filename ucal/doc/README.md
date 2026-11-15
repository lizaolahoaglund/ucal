# UCAL

UCAL means **Unified CAN Abstraction Layer**. The UCAL library supplies vendor-neutral
contracts and optional implementation support. Plugins implement the C contracts without Manager UI headers or UI sources.
Our Qt-based software plugins also statically reuse optional control helpers in
`manager/src/control`; the neutral UCAL library and C interfaces do not depend on Qt.

## Choose the smallest dependency

| Component | Purpose | Dependency |
|---|---|---|
| `include/ucal/BackendPlugin.h` | Native backend DLL entry point and serialized operations | C headers only |
| `include/ucal/FrontendPlugin.h` | Software discovery, inspection, compatibility and launch operations | C headers and UTF-8 JSON |
| `include/ucal/J2534Discovery.h` | Optional package-owned discovery, including per-port and shared-device routes | C headers only |
| `include/ucal/Backend.hpp`, `BackendDispatch.hpp` | Optional C++ backend implementation and wire dispatcher | UCAL, statically linked |
| `include/ucal/ChannelClock.hpp` | Per-channel timestamp-domain mapping without claiming hardware synchronization | C++17 |
| `include/ucal/CanTiming.hpp` | Validation and exact backend-clock conversion for [precise CAN timing](CanTiming.md) | C++17 |
| `Ucal.vcxproj` | Model, protocol, client and backend dispatch library | Windows C++17 compiler |
| `manager/src/control/Control.pri` (repository root) | Manager control-plane helpers; optional for Qt-based software plugins | Qt 5.14.2 |
| `plugin.props`, `backends/*` | MSBuild defaults, original DLL entry/exports and version resources | Matching compiler/architecture |
| `tools/generate-forwarder-def.ps1` | Original generic PE export-forwarding utility | PowerShell and MSVC dumpbin |

Build the static library with `MSBuild ucal/Ucal.vcxproj /p:Configuration=Release
/p:Platform=x64`; use `Win32` for x86. Each concrete plugin project accepts
`/p:UcalRoot=<absolute-ucal-directory>/` when its source is extracted elsewhere.
Link C++ support with the same compiler, architecture and runtime as the plugin.
The C DLL boundary does not depend on C++ or Qt object layouts.

The UCAL directory is self-contained. `ucal/test/UcalCTest.vcxproj` compiles
the public headers as C and checks the original x86/x64 ABI layouts. The Qt helpers
are optional; implementing the C/JSON frontend contract does not require Qt.

## Entry points and operation names

| Contract | Exact export | Operations |
|---|---|---|
| Backend | `UcalBackendGetApi` | `create`, `invoke`, `destroy`; `invoke` uses the versioned UCAL protocol |
| Frontend | `UcalFrontendGetApi` | `scan`, `inspect`, `check_compatibility`, `prepare_launch`, optional `deploy` / `restore_deployment` |
| J2534 discovery | `UcalJ2534DiscoverV1` or `UcalJ2534DiscoverV2` | Bounded records; V2 adds connection routing and optional `UcalJ2534DiscoveryReleaseV2` |

`BackendInterface` is an optional implementation base. `write` retains the legacy
write contract; `submit` and `submitBatch` carry the asynchronous submission
contract. Neither queue acceptance nor a successful write implies physical Tx
completion. CAN/LIN optional operations and status validity retain their existing
capability gates. Device configuration is supplied through the existing channel
configuration; this cleanup adds no device operation.

`UCAL_FRONTEND_CHECK_COMPATIBILITY` remains operation **3**. The launch description
uses `resource_slot` and `reserved_resource_slots` for the existing optional launch
resource reservation. Software-specific validation and engineering configuration
generation belong to the frontend DLL. The Manager checks generic mapping,
inventory freshness, package availability and lifecycle rules.

## Ownership and failures

- Set the API structure's `size`, request a supported ABI, and require the documented
  version, zero reserved fields and required function pointers.
- Buffers and lengths belong to the caller. Supply the documented maximum output
  buffer before a state-changing call; do not retry that call to discover its size.
- Backends serialize calls per instance. `create`/`destroy` delimit resources.
  Frontend operations are synchronous; do not retain input pointers or background work.
- Catch all exceptions inside the DLL. Never pass STL/Qt objects or exceptions across it.
- Backend status values follow the UCAL protocol. Frontend status values retain
  their separate four-value contract documented in `FrontendPlugin.h`; preserve
  vendor error codes in the JSON error result.
- Advertise only implemented optional operations. Device SET and GET support are
  independent. A failed advertised operation returns its actual failure to the host.

## Version and compatibility rules

The existing V1 C structures, calling conventions and numeric operation values
remain fixed. Incompatible changes require a new requested ABI/version and a
separate layout; a new library can continue serving the frozen V1 contract.
Do not append fields to a V1 structure whose loader requires its exact size.
Protocol major/minor negotiation and metadata schema versions remain separate
from the DLL's product/file version. Unknown mandatory contracts fail explicitly.

The UCAL naming transition changes exports, environment prefixes, schema namespaces
and artifact names. Rebuild original plugins and update their metadata together.
Existing third-party J2534 `PassThru*` DLLs and vendor API exports are unchanged.
Saved application data and endpoint identities retain their existing representation.
The opaque V1 identity namespace bytes and wire tags are intentionally frozen.

## Interface responsibilities

| Responsibility | Existing interface | Owner |
|---|---|---|
| Initialize and enumerate live channels | `initialize`, `enumerate`, `enumerateEndpoints` | Concrete backend |
| Open/configure, close and release dead sessions | `open`, `close`, `pruneDeadSessions` | Backend; Broker enforces ownership |
| CAN transmit, receive, events and valid status | `write`, `submit`, `submitBatch`, `read`, `readEvents`, `getStatus` | Backend capability contract |
| Optional LIN operations | `openLin`, `submitLin`, `readLinEvents`, `getLinStatus`, `configureLinSlaveResponse` | Implemented backend capabilities |
| Find software installations and inspect an EXE | `scan`, `inspect` | Concrete software frontend |
| Validate frontend-specific compatibility | `checkCompatibility` | Concrete software frontend |
| Construct a launch description and supported deployment | `prepareLaunch`, optional `deploy`, `restoreDeployment` | Concrete software frontend |
| Persist applications/projects, reserve launch resources and create Windows shortcuts | Control-plane helpers | Manager; no duplicate plugin operation |

Public C++ methods use Qt lowerCamelCase. Frontend V2 JSON retains lower_snake_case.
Frontend V1 retains its original camelCase JSON and numeric operation IDs. The host
requests V2 first and falls back to V1; both share the original C structure layout. The C boundary uses fixed-width types and explicit byte counts;
the optional C++ base is statically linked implementation support, not a DLL ABI.
No additional virtual operation or placeholder extension point is required for
the frozen requirements.

`ProviderQueries.h` describes optional provider records, **not** SAE J2534 or
GMW17753 functions. An explicit engineering configuration can supply an exact
`timing_query` or `discovery_query` export name. The standard J2534 path uses neither;
the host does not infer or probe a vendor-specific export. Previous engineering
query profile tags must be replaced by the concrete provider's export name.

## Source and package boundaries

Concrete implementations live under `frontends/` and `backends/`.
Each plugin owns an independently removable source folder. A source package's `plugin-package.json` describes its
actual build assets. Its runtime `backend.json` / `frontend.json` contains static
metadata only. Discovered endpoints, live state and user preferences are separate.

`endpoint_presentation` supplies a package's family label and, when needed, the
existing channel-handle display conversion. `broker_aliases` and `isolated` describe
legacy direct selection; normal product discovery uses the complete package catalog.
Frontend metadata declares the DLL and the operations Manager can invoke.

Use `tools/Build-UcalPlugins.ps1` to build the optional native projects present in
the checkout. Add `-IncludeTests` to build their test fixtures.
Manual forwarder tools requiring an external input DLL are built explicitly.
Use `test/Test-UcalSourceBoundaries.py` for source/identity/case-catalog checks,
then the maintained deterministic and Qt suites. See the frozen
[requirements](../../doc/baseline/requirements.md) and [tests](../../doc/baseline/tests.md).

See [coding conventions](../../doc/coding_conventions.md) for public names, external-contract exceptions and serialized compatibility rules.

## Source names and legacy loading

Code namespaces are `Ucal` and `Ucal::Manager`. Public C record fields use lowerCamelCase, including `abiVersion` and `protocolMajor`. Function-pointer types use names such as `UcalBackendGetApiFunctionV1`; export the function `UcalBackendGetApi`. The new host retains lookup support for the previous lower-case query names while validating the same binary tables. Rebuild source clients with the current header/type names. JSON keys and wire layouts are unchanged. See [coding conventions](../../doc/coding_conventions.md).
