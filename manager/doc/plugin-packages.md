# Manager plugin packages

## Install and manage packages

Put backend packages in separate folders under `backends/` with a `backend.json` descriptor. Put frontend packages under `frontends/` with a `frontend.json` descriptor. Packages can contain implementation DLLs, dependencies, configuration tools and resources. Manager discovers packages at startup. When adding a package while Manager is running, select **Rescan** on its page.

**Drivers** shows backend names, versions, load status and details. Use **Enabled** (green) or **Disabled** (red) to change the state. Manager stores the setting by package ID in `data/preferences.ini` and applies it at startup and on rescan. **Details** offers **Open vendor configuration tool** when the package declares one. The package supplies the type, DLL path and discovery module. The backend enumerates physical channels at runtime.

**Software** lists installations discovered by frontend packages. Different versions or paths appear separately. **Applications** stores the chosen installation, channel mappings, configuration file and launch arguments. Several applications can refer to one installation.

## Backend types

Use schema `org.ucal.backend-package/5` for new descriptors.

| `type` | Target of `library` |
| --- | --- |
| `ucal` | DLL implementing the UCAL Backend C ABI |
| `j2534-0404-gmw17753` | Vendor DLL implementing J2534 04.04 and GMW17753 |

Manager locates the shared J2534 adapter and matching-bitness worker. These components support isolated, cross-bitness operation and multichannel backends. One vendor can have multiple package instances, each with its own channels.

Schema 5 descriptors contain static metadata and reject a `channels` list. Native UCAL DLLs enumerate their endpoints directly. A J2534 package can declare a `discovery_library` that returns live channels and opaque selectors. The shared adapter holds this inventory in memory and uses the vendor's PassThru calls for traffic. Without a discovery module, the driver default is the sole logical entry and the vendor's configuration selects the port. J2534 04.04 alone cannot enumerate the physical ports. Manager still reads explicit bindings from schema 2 and 3 packages and preserves saved application mappings when their endpoints disappear.

J2534 packages can set `config_list_layout` to `native` when the 04.04 DLL uses native pointer alignment. The default is `packed`. This field describes the DLL's configuration structure, independently of CAN timing.

The optional `manager_version` string records the UCAL Manager release the descriptor targets. It is informational. Manager accepts descriptors without it and does not compare it against the running version. The J2534 package generator reads the version from `UcalManagerPackage.json` when generating a descriptor for a versioned portable package.

See the [backend plugin contract](../../ucal/doc/backend-plugins.md) for fields, admission requirements and the native ABI.

## Vendor configuration tools

A backend or frontend can declare an optional configuration tool.

```json
"configuration_tool": {
  "program": "tools/VendorConfig.exe",
  "arguments": ["--settings", "settings/vendor.json"],
  "working_directory": "."
}
```

Both `program` and `working_directory` accept absolute paths or paths relative to the descriptor. The tool's directory is the default working directory. Arguments pass directly to the process as an array. Relative paths in arguments are resolved by the tool from that working directory. The tool runs only when selected by the user. Manager reports missing files, dependencies and startup errors.

Stop applications using the device if the configuration tool requires it to be idle. After applying settings, reconnect and rescan as directed by the vendor.

## Frontend operations

A frontend package provides discovery and integration for an application family. It can discover several installations, and each installation can be used by several saved applications.

| Operation | Input and result |
| --- | --- |
| `scan` | Returns the `installations` array, with one record per installation. |
| `inspect` | Takes `executable_path` and `source`. Returns an installation record or an empty object for an unsupported executable. |
| `check_compatibility` | Takes the software version, architecture, integration, assignment mode, channel count and `plan`. Returns `frontend` details and the `blockers` array. |
| `prepare_launch` | Takes `plan`, `application_directory`, `temporary_directory`, `language` and `reserved_resource_slots`. Returns a launch descriptor. |
| `deploy` | Takes the plan, workspace and product directories. Performs the package's deployment operation and can return a `message`. |
| `restore_deployment` | Takes the same input as `deploy` and restores the package's deployment. |

Manager handles shortcuts, application duplication and persistence, and generic process launch and stop. The separate VXL API deployment tools use their own application-local loading contract.

Declare only the operations that the package implements. Manager offers the corresponding controls and reports the operation error and vendor code if a call fails. SET and GET are independent device capabilities. Keep configuration controls in the application when it already provides them. Unknown ownership leaves editing disabled in Manager.

## Frontend descriptor example

```json
{
  "schema": "org.ucal.frontend-package/2",
  "id": "example-software",
  "name": "Example software",
  "version": "1.0.0",
  "implementation": "dll",
  "library": "example-frontend.dll",
  "operations": [
    "scan",
    "inspect",
    "check_compatibility",
    "prepare_launch"
  ],
  "maximum_can_channels": 4,
  "full_access": true
}
```

- `id` is a stable identifier of 1 to 127 ASCII letters, digits, periods, underscores or hyphens.
- `maximum_can_channels` is an integer from 1 to 16. `full_access` enables **All compatible channels**.
- The DLL bitness must match Manager. In `library`, `{architecture}` expands to `x86` or `x64`. Unknown fields, duplicate IDs, invalid types and missing DLLs produce scan errors.
- CANoe and PCAN-View use `implementation: dll` and supply their implementation through their packages. See the [support matrix](../../doc/support.md) for tested versions and hardware.

The installation ID is derived from the package ID and executable path. Saved applications preserve their original installation references when an executable disappears. Scanning does not deploy any frontend DLL.

## Frontend DLL ABI

See [FrontendPlugin.h](../../ucal/include/ucal/FrontendPlugin.h). Export the undecorated `UcalFrontendGetApi`. Manager requests V2 (`0x00020000`) first and falls back to V1 (`0x00010000`) when V2 is unsupported. Both versions use the same C table layout and operation numbers 1 through 6, in the order listed above.

Operations exchange UTF-8 JSON objects of up to 1 MiB using caller-owned buffers. Requests also include `manifest_path`. Calls complete synchronously. The plugin releases input pointers and completes its work before returning. Status 0 means success, 1 unsupported, 2 invalid input and 3 operation failure. A failure can return `error` and `vendor_code`. Manager retains the vendor code in its error report.

Installation records contain `executable_path` and can include `display_name`, `version`, `source`, `compatibility`, `compatibility_kind` and an `integration_adapters` string array. The executable must exist so that Manager can read its PE architecture and generate its installation ID.

A launch descriptor contains an absolute `program` and `working_directory`, an `arguments` string array and an optional `environment` object with string values. Manager merges the latter into the system environment. V2 can return a `resource_slot` from 0 to 16. Zero or omission means that no process resource slot is required. A plugin requiring a slot chooses a value from 1 to 16 that is absent from the request's `reserved_resource_slots`. Manager reserves it while the process runs. V1 uses `resourceSlot` and `reservedResourceSlots`. Manager also reads the older `pcanNetIndex` input alias. A launch fails on conflicting aliases, non-integer slots or out-of-range slots. `LaunchPlan::toJson()` defines the `plan` fields for the installation, application and resolved channels.

```json
{
  "program": "C:/Apps/ExampleClient/Client.exe",
  "working_directory": "C:/Apps/ExampleClient",
  "arguments": ["--channel", "1"],
  "environment": {"EXAMPLE_MODE": "can"},
  "resource_slot": 3
}
```

`integrations` maps opaque integration IDs to names. Return those IDs in `integration_adapters`. For example, `example_route` and `exampleRoute` are distinct IDs. The spelling of environment variable names and language keys is preserved too.

The [frontend test fixture](../../ucal/test/plugins/frontend/mocks/Mock.cpp) illustrates the contract without vendor dependencies.

## Portable data and application state

Manager stores `profiles.json`, preferences, software search paths, backend settings, launch records and diagnostics in `data/` beside the program. At first startup Manager can copy existing local settings, leaving the originals in place. Files already present in the portable directory take precedence. Keep `data/` when upgrading or rolling back. Run the package from a writable location.

Software discovery checks the registry and installation paths supported by each frontend. When a supported executable is added manually, Manager remembers its path for rescanning. Removing a Software entry leaves the executable installed and allows a future scan to find it again. You can remove its entry after removing application references. **Create application** is unavailable for unsupported or missing software. Existing application settings remain available.

The default project includes all applications and cannot be deleted. Custom projects can be renamed or deleted without removing their applications. The channel editor identifies the driver, device and physical port. Removing a logical channel requires confirmation. Saving renumbers the following rows.

The hardware list comes from backend metadata and live channel inventory. **Drivers → Details** displays the implementation DLL name, path, file version and any declared configuration tool. The status bar shows operation messages, service and inventory status, and application and channel counts.

## Metadata and inventory

`backend.json` and `frontend.json` describe the plugin. Runtime devices, software installations and online state are discovered again at startup. During a run, a removed device can appear offline. Only endpoints referenced by saved applications are retained between launches. A missing saved endpoint appears as **Not discovered · Kept binding** in its original mapping. Workspace schema 3 in `profiles.json` stores the descriptions needed for saved bindings. Schema 1 and 2 files migrate with their endpoint IDs and application order intact.

The hardware usage list shows which applications reference a channel. Use an application's settings to modify its mappings. When renaming a project, Manager edits its saved name without including the list's all-applications indicator.

## Driver state, projects and channel filters

Use the **Enabled** or **Disabled** control on **Drivers**. Disabling a driver stops discovery and new connections while allowing active connections to close normally. Package files and saved mappings remain available. Re-enabling the driver triggers fresh discovery. The preference applies to all projects in the portable directory.

Check applications in project management to edit custom project membership. Saved projects retain their existing members, each of which can be individually selected or cleared. The default project always includes all applications.

**All compatible channels** selects currently available channels in stable order, subject to the frontend limit. CANoe selects up to 16 channels, and PCAN-View selects one. Use **Selected channels** for fixed ports or order. Switching to automatic mode preserves saved explicit mappings.

The Hardware filter matches drivers, devices, serial numbers and channel numbers. Matching ignores case and requires all space-separated terms. Clearing the filter restores the list. The filter affects display only and resets on restart.

## J2534 configuration tools and channel discovery

A `configuration_tool` entry exposes **Drivers → Details → Open vendor configuration tool**. Manager starts the declared program. Use the vendor's documented steps to apply settings, reopen connections and rescan when necessary.

PEAK and Vector discovery modules use the public PCAN-Basic and XL inventory interfaces, respectively. A separate UCAL process manages the discovery SDK lifecycle. Discovery reads the inventory without running the vendor configuration program, opening a traffic session, sending frames or changing the vendor's default mapping. Standard PassThru calls in the vendor DLL handle the traffic. Discovery ABI V2 describes either separate Open calls per port or multiple Connect calls on a shared DeviceID. The backend manages the resulting sessions separately.

A saved physical mapping uses its vendor-specific selector even if the vendor's default port changes. The driver can still apply its configured defaults for the selected port, including termination and timing. Explicit settings require the driver's corresponding interface. Keep fixed CAN FD sample-point declarations consistent with the vendor configuration. Manager has no automatic synchronization for parameters that the driver cannot read or write through a documented API.

Use `lower_snake_case` fields in backend schema 5 and frontend schema 2. Manager reads older metadata, but rejects multiple aliases that normalize to the same field. Frontend ABI V2 uses the same JSON field style. V1 retains its original field spelling.
