# Module layout

The checkout is organized by module. Each module owns its implementation, tests,
documentation, build projects and static package metadata.

| Directory | Responsibility |
| --- | --- |
| `ucal/include/ucal` | Public neutral C/C++ contracts |
| `ucal/src` | Neutral model, IPC, Broker, plugin host and standard J2534 template |
| `ucal/test` | Core, protocol, generic plugin/transport mocks and hardware-contract runners |
| `ucal/tools`, `ucal/doc` | Public export/deployment helpers and core interface documentation |
| `manager/src`, `manager/res` | Qt control plane and original presentation resources |
| `manager/test`, `manager/doc` | Qt/package regression, fixed case catalog and Manager guides |
| `frontends/<id>` | Independent API compatibility or installed-software module |
| `backends/<id>` | Independent native hardware adapter or J2534 discovery module |
| `test` | Cross-module integration, source checks and regression orchestration |
| `tools` | Shared build, package, export and application-process orchestration |
| `doc` | Architecture, requirements, verification and coding conventions |
| `dist`, `out` | Generated runtime and local build/test evidence; not source |

## Module contents

Use singular `test` both at the root and inside a module. `frontends` and `backends` are classification
directories; their child is the independently removable module. A module may have
`src`, `include`, `test`, `doc`, `res` and `tools` when it actually has such content.
Do not create empty directories to complete a template.

Public headers available to consumers belong in `include`. Private headers remain
with their implementation under `src`. A native plugin builds against the public
UCAL include/library boundary and never includes another plugin's private header.
The headers under `ucal/include/ucal` belong to the UCAL module. Consumers add
`ucal/include` to their include path and write `#include <ucal/Model.hpp>`; the
inner `ucal` directory is the include namespace, not a second implementation module.
The Qt software integration uses the common control-plane build support supplied
with Manager; its C DLL boundary is defined by UCAL.

`backend.json`, `frontend.json` and `plugin-package.json` are static build/runtime
metadata at module roots. Source assets are copied from their owning module into
the portable package. Installed software, discovered channels, online state and
process state are runtime data, not static metadata.

## Optional modules and tests

Build scripts discover module descriptors from `frontends` and `backends`. Product
projects live at the module root. Native unit/mock projects live under its `test`
folder and are included only when requested. Removing a complete optional module
therefore removes its tests and package assets without editing the core solution.

J2534 provider-specific discovery mocks and scenario metadata belong to each
backend. The UCAL test runner exercises the shared C discovery contract; its
runner discovers each available module's `test/discovery.json`. Run the public
core, API deployment and Manager suites using the [regression guide](../test/Running.md).

The [Manager test catalog](../manager/test/cases.json) lists the functional
cases. Its Windows Qt suite builds and exercises both software frontend
plugins with original fixtures. The source isolation suite builds the core
and Manager with optional modules removed.

## Launch and package

Manager requests a launch plan from the installed software frontend plugin.
That plugin resolves its required support binaries and prepares the launch.
Backend packages declare runtime dependencies in their own metadata. Applications
configure the mapped channels through their native settings and the capabilities
reported by the selected endpoint.

Native builds continue to use MSBuild, and Qt 5.14.2 builds use qmake. Runtime
package paths and versioned ABI/persisted identities are independent of the source
layout. See [regression entry points](../test/Running.md).

The public source includes the frontend plugin contracts and support ABI
fixtures. Installed CANoe and PCAN-View integrations use the separately
distributed compatibility binaries described in the [package guide](../manager/doc/PackageLifecycle.md).
