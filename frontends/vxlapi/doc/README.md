# VXLAPI frontend

This API frontend supplies the public Vector XL consumer boundary. It builds
`vxlapi.dll` for x86, `vxlapi64.dll` for x64 and a matching `UcalXlDiscovery`
sidecar. The API DLL forwards calls to the adjacent `UcalVectorSupport.dll` or
`UcalVectorSupport64.dll`. That separately distributed compatibility implementation
translates into UCAL and reaches the selected hardware backend; it does not require
a Vector hardware backend.

```text
XL API client -> vxlapi DLL -> Vector support DLL -> UCAL Broker -> selected backend
```

Public source includes declarations, channel catalog, discovery, forwarding shell
and deployment assets. The shared XL runtime and CANoe loading-profile implementation
are binary support components; their private source is omitted from public source
exports. The separate `frontends/canoe` source
group supplies the Manager software discovery/launch profile.

Build `UcalXlCompatibility.vcxproj` with MSBuild for Win32 or x64. The public shell
and discovery build independently of private support sources; running the API needs
the matching support binary.
External builds can set `UcalRoot` to the public UCAL source/include/library root.
No vendor SDK headers or runtime are included. Support binaries use the terms in
[CompatibilitySupport.txt](../../../licenses/CompatibilitySupport.txt).

Explicit Classic/FD TSEG and SJW settings use the neutral advanced-timing contract.
Rate-only calls retain the backend's timing selection. An explicit timing request
requires a backend advertising `CapabilityCanAdvancedTiming`; unsupported timing
fails at activation rather than being reduced to bitrate and integer sample point.
See [precise CAN timing](../../../ucal/doc/CanTiming.md).

## Deploy an API client

Use an application copy under your control. The helper validates a static XL import boundary and exact exported names, matching x86/x64 architecture, backup and reversible DLL replacement. It leaves the EXE unchanged.

```powershell
$helper = '.\frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1'
& $helper -Action Inspect -TargetExecutable 'C:\client-copy\demo.exe'
& $helper -Action Apply -TargetExecutable 'C:\client-copy\demo.exe' `
  -FrontendDll '.\out\x64\Release\vxlapi64.dll' `
  -DiscoveryDll '.\out\x64\Release\UcalXlDiscovery64.dll' `
  -BackupDirectory '.\out\client-backups\xl-demo'
```

Start a Broker configured with the desired backend, then run the client in the same
`UCAL_BROKER_INSTANCE` environment. All three DLLs must match the client's bitness.
The helper selects the support DLL adjacent to `-FrontendDll`; use `-SupportDll`
to select another matching copy. The Broker can independently use a supported
backend worker architecture. The application owns normal XL timing/configuration.

```powershell
& $helper -Action Restore -TargetExecutable 'C:\client-copy\demo.exe' `
  -BackupDirectory '.\out\client-backups\xl-demo'
```

The helper refuses a changed EXE, changed deployed DLL or damaged backup during restore. It does not write Windows system directories. Programs using explicit system paths, signed plugin policies or different APIs need their own loading assessment. Dynamic loading is possible when the program resolves the adjacent UCAL DLL, but the static-import helper does not certify that route. An exported function is not a guarantee that every hardware feature is supported; unsupported operations return an error.

Restore also accepts the earlier two-file backup format. It restores only that
backup's declared files and does not infer ownership of a support DLL added later.

The [API regression guide](../../../test/Running.md#independent-api-clients)
lists the tests that run from public source and the matching support DLLs needed
for full x86 and x64 API traffic tests. Verify SDK demos and other applications
separately. See [support scope](../../../doc/support.md).
