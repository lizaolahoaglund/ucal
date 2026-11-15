# PCAN-Basic application-local deployment

Run these commands from the UCAL source checkout. Start Manager from the
extracted portable package and pass that package's API DLL to the deployment
helper. You can also pass a locally built API DLL of the same architecture.
Prepare a writable client copy and keep its official installation unchanged.

1. Open UCAL Manager and enable the desired hardware backends. A normally launched client uses the default per-user Broker.
2. Inspect the client's static `PCANBasic.dll` import boundary.
3. Deploy the DLL matching the client's bitness. The example is for x64; use the x86 folder for an x86 application, even on x64 Windows.

```powershell
$helper = '.\frontends\pcan_basic\tools\Set-UcalPcanBasicClientDeployment.ps1'
$package = 'C:\path\to\extracted-ucal'
& $helper -Action Inspect -TargetExecutable 'C:\client-copy\demo.exe'
& $helper -Action Apply -TargetExecutable 'C:\client-copy\demo.exe' `
  -FrontendDll (Join-Path $package 'frontends\pcan_basic\x64\PCANBasic.dll') `
  -BackupDirectory 'C:\client-backups\pcan-demo'
```

The helper checks the exact required exports and bitness, backs up an existing adjacent DLL and leaves the EXE unchanged. Copy failure rolls back the DLL. Keep the backup directory outside the client folder.

4. Start the client and choose its API channel. The application supplies timing with `CAN_Initialize` or `CAN_InitializeFD`. Custom Broker instances require the same `UCAL_BROKER_INSTANCE` environment in the client.
5. Close the client, then restore the original DLL with the same backup directory.

```powershell
& $helper -Action Restore -TargetExecutable 'C:\client-copy\demo.exe' `
  -BackupDirectory 'C:\client-backups\pcan-demo'
```

Changed EXEs, changed deployed DLLs and damaged backups stop restoration with an error. The helper does not write Windows system directories. A backend must load its installed vendor runtime, never this UCAL compatibility DLL.

PCAN-Basic FD timing includes both sample points. A generic J2534 backend without a supported matching FD sampling contract rejects that request; Classic CAN remains usable. Other unsupported options also return an error.

The helper checks static native imports. Managed P/Invoke, dynamic loaders and fixed DLL paths require separate loading verification. Each SDK demo still needs a run for its actual API subset. API-only packages do not create a Manager software installation row.

Source/build instructions and reusable regression are in the source repository under `frontends/pcan_basic` and `test`.
