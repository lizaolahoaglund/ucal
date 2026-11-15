# API frontend deployment

UCAL's VXLAPI and PCAN-Basic frontends allow an application that calls these APIs to reach a selected UCAL hardware backend. They are independent source packages under `frontends/vxlapi` and `frontends/pcan_basic`, distinct from the native hardware backends with the same vendor API boundary.

| Application API | x86 deployment files | x64 deployment files |
| --- | --- | --- |
| Vector XL | `vxlapi.dll`, `UcalXlDiscovery.dll`, `UcalVectorSupport.dll` | `vxlapi64.dll`, `UcalXlDiscovery64.dll`, `UcalVectorSupport64.dll` |
| PCAN-Basic | `PCANBasic.dll` | `PCANBasic.dll` |

The Vector support DLL is a separately distributed binary. Use the one matching
the VXLAPI build and client architecture.

The portable product keeps the ready-to-deploy DLLs under `frontends/vxlapi/{x86,x64}` and `frontends/pcan_basic/{x86,x64}`. Deployment helpers and their supporting modules belong to the source repository, under each source module's `tools` directory; they are not included in the user ZIP. Obtain the matching public source attachment to run those helpers, and pass the API DLL from the extracted user package. API-only packages do not add a Manager software installation or application row.

## Application-local workflow

1. Prepare a writable copy of the client and its dependencies. Use the client's bitness to choose the frontend; an x86 program needs x86 DLLs even on x64 Windows.
2. Run the appropriate deployment helper with `-Action Inspect`. The helper reads static PE imports without loading or changing the EXE. It checks the recognized DLL boundary and a supported initialization entry point.
3. Run `-Action Apply` with the matching frontend files and an external backup directory. It validates every imported symbol against the DLL exports and checks all file architectures. Missing required functions stop deployment before files are changed. The helper backs up existing adjacent DLLs and rolls back an interrupted copy.
4. Open UCAL Manager so its Broker is running with the enabled backend packages. A client launched normally can use that default per-user Broker. A developer who starts a separate Broker must use the same `UCAL_BROKER_INSTANCE` in the client environment. Choose the exposed API channel in the client; API initialization/configuration calls supply the application's timing and supported options.
5. Close the client and run `-Action Restore` using the same target EXE and backup directory. Restore checks the EXE, deployed file hashes and backups, then restores the original files. A changed file is reported rather than overwritten.

The deployment helper writes only adjacent DLLs and its backup records. It does not modify the executable or install a system driver. Use owned application copies for verification; keep official vendor installations and Windows system directories unchanged.

Commands and file details: [VXLAPI frontend](../frontends/vxlapi/doc/README.md), [PCAN-Basic frontend](../frontends/pcan_basic/doc/README.md).

## Loading and capability boundaries

The automatic helper qualifies a named static DLL import. A managed P/Invoke, `LoadLibrary`, explicit system path or signed-loader policy needs separate loader verification; copying an adjacent DLL does not prove that the program loads it. An API's export presence also does not guarantee support for every parameter used by an SDK demo.

All conforming backends share the same routing path. Requested options must still be supported by the selected backend. In particular, PCAN-Basic FD initialization contains nominal and data sample points. Generic J2534 does not offer a universal FD sampling setter, so a runtime without a supported matching sampling contract rejects that request. VXL can provide rate-only FD settings when its caller does not request explicit sample points. Requested values are never silently discarded to make a route appear successful.

The software profiles for CANoe and PCAN-View retain their separate loading and evidence boundaries. The PCAN-View executable bridge is not the general PCAN-Basic frontend. See [support scope](support.md) and [reproducible API regression](../test/Running.md#independent-api-clients).
