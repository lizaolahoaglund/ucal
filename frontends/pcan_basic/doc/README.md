# PCAN-Basic frontend

This independent API frontend builds UCAL's own `PCANBasic.dll` for x86 and x64. It implements public `CAN_*` calls and routes them through the UCAL Broker to the selected backend, independently of whether the hardware is PEAK, Vector or another conforming backend.

```text
PCAN-Basic client -> UCAL PCANBasic.dll -> UCAL Broker -> selected backend
```

The PCAN-Basic hardware backend under `backends/pcan_basic` has the opposite responsibility: it calls an installed vendor runtime. These libraries must have separate paths. Do not configure a backend to load the UCAL compatibility DLL as its vendor driver.

Build `UcalPcanBasic.vcxproj` with MSBuild for Win32 or x64. Set `UcalRoot` when building the copied source group outside the repository. The package includes original ABI declarations, source, metadata and deployment tools; it does not include vendor runtime binaries or SDK material. It does not create a Manager software installation row. PCAN-View's version-pinned executable bridge is a separate software profile.

## Deploy an API client

Use a writable application copy. The helper verifies its static `PCANBasic.dll` import boundary, architecture and required exported functions, then backs up and replaces the adjacent DLL without editing the EXE.

```powershell
$helper = '.\frontends\pcan_basic\tools\Set-UcalPcanBasicClientDeployment.ps1'
& $helper -Action Inspect -TargetExecutable 'C:\client-copy\demo.exe'
& $helper -Action Apply -TargetExecutable 'C:\client-copy\demo.exe' `
  -FrontendDll '.\out\x64\Release\PCANBasic.dll' `
  -BackupDirectory '.\out\client-backups\pcan-demo'
```

Run a Broker with the desired backend and launch the client in the same `UCAL_BROKER_INSTANCE` environment. The application calls `CAN_Initialize` or `CAN_InitializeFD` to supply its own timing. Classic CAN, CAN FD/BRS, channel enumeration and supported status/configuration operations use the common UCAL device interface. Unsupported optional parameters, event registration or filter operations return their defined error; they are not silently accepted.

```powershell
& $helper -Action Restore -TargetExecutable 'C:\client-copy\demo.exe' `
  -BackupDirectory '.\out\client-backups\pcan-demo'
```

The helper refuses changed files or damaged backups and does not write system directories. Static import checks do not cover a managed P/Invoke or `LoadLibrary` client; such a client can use the API DLL if its loader resolves the adjacent library, but requires separate loading verification. The exact API subset used by each SDK demo must be tested before claiming support for that demo.

The maintained original static-import client exercises enumeration, initialization, Classic/FD+BRS traffic and clean close. Deterministic regression is hardware-free and distinct from physical device or installed-application acceptance. See [regression](../../../test/Running.md) and [support scope](../../../doc/support.md).

Channel handles retain their endpoint identity for the lifetime of the frontend DLL. Missing channels remain unavailable slots; the attached-channel list omits them while keeping surviving handles unchanged. Restored endpoints reuse their original slots. Up to 16 slots can be learned in one process.

PCAN-Basic FD timing preserves the nominal and optional data TSEG/SJW tuple, including fractional sample points such as 87.5%. A generic J2534 backend without the neutral advanced-timing capability rejects that initialization with `PCAN_ERROR_ILLOPERATION`; Classic CAN can still work. Native PCAN-Basic and Vector XL accept exact timing within their API limits. Invalid SJW values are rejected before opening a channel. See the [timing contract](../../../ucal/doc/CanTiming.md).
