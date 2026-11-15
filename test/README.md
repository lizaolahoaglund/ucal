# Cross-module tests

Module-owned tests use singular `test` inside `ucal`, `manager` or a concrete
frontend/backend. This directory contains only source/architecture checks,
composed integration, shared API acceptance catalog and regression orchestration.

- `Run-Tests.ps1`: selects the native gate matching the available source scope.
- `integration/Test-UcalPublicCore.ps1`: public core, backend and API fixtures.
- `Test-UcalDeterministic.ps1`: public native and Manager regression runner.
- `Test-UcalSourceBoundaries.py`, `Test-UcalNaming.py`, `Test-UcalLayout.py`: source contracts.
- `Test-UcalIsolation.ps1`: clean-source/removable-module and standalone build proof.
- `Test-UcalApiFrontends.ps1`, `Test-UcalApiClientDeployment.ps1`, `cases.json`: cross-module API routes.

See [entry points](Running.md), the [Manager suite](../manager/test/README.md),
[core hardware contracts](../ucal/test/hardware/README.md), and module-local test
descriptions. Reusable source stays in Git; builds, captures and station inputs
stay under ignored output. Application and hardware acceptance remain opt-in.

The native runner builds Debug and Release public components for x86 and x64 and
runs their deterministic fixtures. Installed applications have separate opt-in
tests, which require the vendor software and any applicable support binaries.
