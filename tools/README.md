# Build and package tools

- `Build-UcalDist.ps1`: orchestrate the native and Qt builds and assemble the portable product.
- `Build-UcalPlugins.ps1`: build optional native plugin source groups.
- `Build-UcalManagerFrontends.ps1`: build the Qt software-plugin DLLs.
- `UcalToolchain.psm1` / `DefaultCppToolset.props`: shared compiler discovery and MSBuild setup.
- `packaging`: source export, product assembly, manifest and ZIP generation.
- `generate-forwarder-def.ps1`: existing forwarding entry point into the UCAL library's export generator.

See the [regression guide](../test/Running.md) for build and test commands.
Manager icon tooling is in `manager/tools`.
