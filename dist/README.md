# Portable UCAL Manager output

`tools/Build-UcalDist.ps1` assembles the portable product here. See the [package guide](../manager/doc/PackageLifecycle.md) for the separate compatibility binary and notices inputs. The directory contains `UcalManager.exe`, Qt runtime libraries and platform/image plugins, UCAL host runtimes, frontend/backend DLLs, and their static JSON descriptions/resources. Source JSON templates stay in the owning plugin directory and are copied during assembly. Application data is created separately under `data/` at runtime.

Generated product files are ignored by Git. Use a new/empty destination for assembly; existing application data and previous packages are not overwritten. Pass `-DistDirectory` for an alternate product destination under `dist/` or `out/`.
