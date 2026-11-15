# Reversible application-local VXL deployment

`Set-UcalVxlClientDeployment.ps1` is for user-authorized ordinary third-party
programs that expose a technically replaceable public Vector XL DLL boundary. It does
not identify applications by product name, publisher, filename allowlist, or hash.

The tool parses the target executable without loading it. It requires a matching x86
`vxlapi.dll` or x64 `vxlapi64.dll` import, verifies every named XL import against the
matching UCAL frontend's PE export table, and validates the support and discovery DLLs.
Apply copies all three DLLs beside the executable. The executable is left unchanged.
The tool requires a static PE import. Applications that resolve XL dynamically need
separate loader verification.

From the source checkout, inspect the writable client copy:

```powershell
.\frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1 `
  -Action Inspect -TargetExecutable C:\path\to\client.exe
```

Obtain the matching separately distributed Vector support DLL before applying.
The helper looks for it next to `-FrontendDll`. Use `-SupportDll` when it is in
another directory. Apply with a backup outside the application directory, then
restore with the same executable and backup directory:

```powershell
.\frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1 -Action Apply `
  -TargetExecutable C:\path\to\client.exe `
  -FrontendDll out\x64\Release\vxlapi64.dll `
  -DiscoveryDll out\x64\Release\UcalXlDiscovery64.dll `
  -BackupDirectory out\vxl-client-backups\client-review

.\frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1 -Action Restore `
  -TargetExecutable C:\path\to\client.exe `
  -BackupDirectory out\vxl-client-backups\client-review
```

Restore verifies the executable, installed files, and original backups before it
writes. A changed executable or changed deployed DLL is reported for manual review
instead of being overwritten. Windows directories are always outside this tool's
write boundary.
