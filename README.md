# Unified CAN Abstraction Layer (UCAL)

UCAL provides a neutral CAN/CAN FD model and Broker between external software contracts and hardware drivers. **UCAL Manager** discovers supported software installations, loads backend packages and saves per-application channel mappings. The executable is `UcalManager.exe`. UCAL requires hardware with ISO CAN FD support. A compatible device can run either Classic CAN or CAN FD sessions.

Independent public API frontends provide VXLAPI and PCAN-Basic DLLs for application-local deployment. Software plugins cover the separate CANoe and PCAN-View engineering profiles. Public hardware source groups cover native PEAK PCAN-Basic and Vector XL plus their independent J2534 discovery adapters. The neutral J2534 adapter supports 04.04 + GMW17753 raw CAN FD. Vendor runtimes and restricted SDK material are not included. Read [support scope](doc/support.md) before selecting an integration profile.

Serial-identified Vector hardware and Virtual CAN mappings use adapter/port identity independently
of XL global channel order. See the [Vector backend contract](backends/vxlapi/doc/Backend.md).

PCAN-View's engineering profile adapts its timing editor to the selected endpoint:
bitrate/sample-point targets remain usable without precise-timing support;
clock/TSEG/SJW editing requires the backend's advanced-timing capability.
CANoe's engineering profile uses precise timing with capable endpoints. Other
endpoints use bitrate and nearest whole-percent sample-point targets derived from the
application configuration. The backend selects its controller timing.
CANoe Setup owns explicit sample-point and controller-timing inputs.
On the first CANoe application launch, Manager prepares an owned launch copy from
the user's installed CANoe and Vector runtime. For CANoe 11 and 12, Manager selects a
shorter launch-copy directory when the local application data path is deep.
Vendor executables and DLLs are
copied locally and are never included in the UCAL release ZIP; UCAL's original
frontend/support modules are included. No developer build tools are needed.
Manager synchronizes packaged compatibility DLLs before starting its managed
CANoe launch copy. Close CANoe instances using that copy before changing packages;
previous DLLs are retained for rollback, while forwarding shells and saved
application settings are preserved.

- [Source layout and build](doc/layout.md)
- [API frontend deployment](doc/api-frontends.md)
- [Architecture](doc/architecture.md) and [coding conventions](doc/coding_conventions.md)
- [C and plugin contracts](ucal/doc/README.md), [backend packages](ucal/doc/backend-plugins.md)
- [CANoe](frontends/canoe/doc/Launch.md), [PCAN-View](frontends/pcanview/doc/Launch.md), [optional J2534 backends](manager/doc/j2534-user-guide.md) and [portable package](manager/doc/PackageLifecycle.md)
- [Requirements](doc/baseline/requirements.md), [Windows build and regression](test/Running.md)

The maintained native build uses MSBuild; Qt 5.14.2 projects use qmake. `tools/Build-UcalDist.ps1` assembles the portable runtime in `dist`. Build caches and local evidence live under ignored `out`. Hardware-free regression builds its fixtures and product from source for x86/x64; installed-product and physical gates remain opt-in.

The public components use **LGPL-3.0-only WITH Independent-modules-exception**.
Changes to covered components retain their source obligations when distributed;
independent applications and plugins may use their own licenses. Read the
[license scope](LICENSE_SCOPE.md) and [dependency notices](THIRD_PARTY_NOTICES.md).
CANoe/Vector and PCAN-View compatibility engines are separately licensed binary
support modules; their special implementations are maintained outside the public
source delivery set. Their interfaces and Manager profiles remain public.

The user ZIP contains runtime components and license information. Developer tools
remain in Git; the exact covered public source is supplied as a matching release
attachment identified by `SOURCE.txt`. Backend runtime-path overrides
belong to Manager's `data/backend-plugins` settings, not the installed plugin files.

To build the public components and run their regression tests on Windows, follow
the [regression guide](test/Running.md). To assemble a portable package, see the
[package guide](manager/doc/PackageLifecycle.md) for dependency notices and the
matching source archive.
