# VXLAPI application-local deployment

These developer tools are maintained in the Git source checkout and are not included in the end-user ZIP. Run the commands from the source checkout and set `$package` to the extracted UCAL portable package. Use a writable copy of the target application and keep its official installation unchanged.

1. Open UCAL Manager and enable the hardware backends to expose. Normally launched API clients use the Manager's default per-user Broker.
2. Inspect the client's static XL API import. The helper reads the EXE without loading or changing it.
3. Deploy the DLLs matching the client's architecture. The example below is for an x64 client; use the x86 folder, `vxlapi.dll` and `UcalXlDiscovery.dll` for an x86 client.

```powershell
$helper = '.\frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1'
$package = 'C:\path\to\extracted-ucal'
& $helper -Action Inspect -TargetExecutable 'C:\client-copy\demo.exe'
& $helper -Action Apply -TargetExecutable 'C:\client-copy\demo.exe' `
  -FrontendDll (Join-Path $package 'frontends\vxlapi\x64\vxlapi64.dll') `
  -DiscoveryDll (Join-Path $package 'frontends\vxlapi\x64\UcalXlDiscovery64.dll') `
  -BackupDirectory 'C:\client-backups\xl-demo'
```

The helper selects `UcalVectorSupport64.dll` adjacent to the selected frontend
(`UcalVectorSupport.dll` for x86), or accepts an explicit `-SupportDll`. All three
DLLs must match the client architecture. It checks bitness and every required
named import before copying files, backs up existing adjacent DLLs and rolls back
a copy failure. Keep the backup directory outside the client folder.

4. Start the client and choose the UCAL-exposed XL channel. The application supplies its timing and supported configuration through XL calls. A custom Broker instance requires the same `UCAL_BROKER_INSTANCE` environment in the client.
5. Close the client and restore the exact original files with the same backup directory.

```powershell
& $helper -Action Restore -TargetExecutable 'C:\client-copy\demo.exe' `
  -BackupDirectory 'C:\client-backups\xl-demo'
```

Restore refuses a changed executable, changed deployed DLL or damaged backup. Windows system directories are outside the helper's write boundary.

The helper qualifies static native imports. Managed P/Invoke, dynamic loading, fixed DLL paths and signed-loader policies need separate loading verification. Support for an exported function does not imply support for every hardware option. Unsupported parameters return an error. API-only packages do not create a software installation row in Manager.

Source/build documentation and reusable regression are in the source repository under `frontends/vxlapi` and `test`.
