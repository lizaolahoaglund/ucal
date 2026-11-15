# PCAN-View integration prerequisites

The public tree includes the PCAN-View Manager profile, launcher and
[binary support interface](../../doc/Support.md). Installed acceptance requires
a supported PCAN-View executable and the matching compatibility support and
adapter binaries.

From the source checkout, first run the
[public native regression](../../../../test/Running.md#tests-from-public-source).
To rerun just the x64 support-interface fixture with its Release build outputs:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File frontends/pcanview/test/Test-UcalPcanViewSupport.ps1
```

See the [support matrix](../../../../doc/support.md) for the qualified versions.
