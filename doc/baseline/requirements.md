# UCAL requirements

UCAL connects external software to CAN hardware. UCAL Manager discovers software and hardware, saves channel mappings and launches applications.

## Product and plugin boundaries

- The data path has three layers: compatibility frontends, the vendor-neutral core and Broker, and hardware backends. UCAL Manager provides the control plane.
- Plugin packages provide static metadata, implementation DLLs and optional resources or vendor configuration tools. Manager discovers packages at startup and can rescan them.
- Backend types include native UCAL and J2534 04.04 with GMW17753. Multiple J2534 packages share the transport adapter. A package can expose multiple channels through separate Open calls or through separate Connect calls on a shared DeviceID.
- Vendor-specific discovery belongs to the backend plugin and uses documented interfaces. An optional vendor configuration executable is separate from Manager's live channel inventory.
- Native and J2534 CAN backends require ISO CAN FD-capable hardware. Qualified hardware can also run Classic CAN sessions.
- Enumerate channels and online state at runtime. Keep user preferences and transient inventory out of package metadata.

## User interface and operations

| ID | Required behavior |
| --- | --- |
| UI-01 | The sidebar orders Applications, Software, Drivers and Hardware. Navigation entries and Settings have icons, and the product name and version are aligned at the bottom. |
| UI-02 | The Applications overview shows the application count. Its empty state guides users through hardware discovery, software discovery and application creation. |
| UI-03 | Applications provides a creation entry point. Software provides Rescan and Add EXE. Unsupported software cannot create an application. |
| UI-04 | Software entries represent installations. Rescanning can rediscover a forgotten installation. Users can view the number and list of applications that use it. |
| UI-05 | Drivers lists concrete packages with their names, vendors, DLL and package versions, paths and capabilities. The details include the vendor configuration tool when declared. |
| UI-06 | Enabled is green and Disabled is red. The choice persists, and disabling stops discovery and new opens while allowing existing sessions to close normally. |
| UI-07 | Hardware endpoints show their source driver. The filter accepts multiple terms and resets on restart. The hardware list comes from backend inventory. |
| UI-08 | Users can create, rename, switch and delete projects and edit their application membership. The default project includes all applications and cannot be deleted. Applications exist independently of projects. |
| UI-09 | Applications support managed and native launch, showing the running window, duplication, deletion, shortcuts, compatibility checks, opening the software location and reversible deployment. |
| UI-10 | Application actions appear in the order Launch, Settings and More. Development progress and diagnostic capability matrices stay out of the UI. |
| UI-11 | Settings covers language, tray and close behavior, startup preferences and local portable data and diagnostics. Updates are unavailable without an update source. |

## Channel mappings and configuration ownership

| ID | Required behavior |
| --- | --- |
| MAP-01 | Selected channels can be added and removed by row. Removal requires confirmation. The frontend sets the limit: 16 for CANoe and one for PCAN-View. |
| MAP-02 | All compatible channels fills the frontend limit in stable order, with up to 16 channels for CANoe and one for PCAN-View. Preview and launch use the same inventory. |
| MAP-03 | Automatic mode preserves saved explicit mappings. Removing a device leaves its mapped slots offline without compacting, reordering or replacing them. |
| MAP-04 | An application may map an endpoint ID once. The selector marks assigned endpoints, and an import with duplicates identifies all conflicting slots and blocks save and launch. |
| MAP-05 | Applications may reference the same endpoint. Runtime leases determine whether it is busy. Endpoint names alone do not establish that two backends refer to the same physical port. |
| MAP-06 | The application usage list on Hardware is read-only. |
| CFG-01 | The application owns bitrate, sample point, termination, operating mode and hardware filters when it provides controls for them. Manager hides competing controls or displays them as read-only. |
| CFG-02 | Manager provides a fallback only for required parameters without an application control and with backend support. Unknown ownership leaves editing disabled. |
| CFG-03 | SET and GET are independent. A SET-only capability can be used. A failed operation reports the actual error and vendor code. |
| CFG-04 | Basic information shows properties. Integration and Launch holds the configuration file and startup parameters. Compatibility checking has a single entry point. |

## Runtime, storage and safety

- Windows USB changes trigger background discovery with debouncing and a follow-up scan. Stale results cannot overwrite newer topology or driver state. Applying edits reconciles with the current inventory. On Linux, discovery is manual.
- Launch requires current inventory, an available software installation and a complete, valid mapping. Unknown, offline, duplicate or over-limit mappings produce a visible error.
- Applications and running instances retain independent mappings and temporary launch configuration. Native launch remains available.
- Portable settings are stored beside the program in `data/`. Writes are atomic and preserve backups. Manager supports recovery from corrupted settings and rejects newer schemas it cannot read. System and vendor installation directories are protected from writes.
- DLL boundaries use plain C or serialized bytes, with caller-owned buffers and explicit operation and process lifetimes.
- Endpoint capabilities determine supported frames, timestamps, transmit events, filters and controller state. A transmit-completion event requires physical completion reported by the driver.

## Acceptance scope

CANoe 11 and 12 use the application-local RuntimeKernel and XL route. PCAN-View uses a separate, version-scoped compatibility module. See the [support matrix](../support.md) for tested software versions, devices and communication scope.

[Plugin packages](../../manager/doc/plugin-packages.md) · [Configuration ownership](../architecture.md) · [Test scope](tests.md)
