[CmdletBinding()]
param(
    [string]$DeploymentScript = 'frontends\vxlapi\tools\Set-UcalVxlClientDeployment.ps1',
    [string]$FrontendX64 = 'out\x64\Release\vxlapi64.dll',
    [string]$DiscoveryX64 = 'out\x64\Release\UcalXlDiscovery64.dll',
    [string]$FrontendX86 = 'out\Win32\Release\vxlapi.dll',
    [string]$DiscoveryX86 = 'out\Win32\Release\UcalXlDiscovery.dll'
)

# Keep the existing entry point for callers of the VXL acceptance suite.
& (Join-Path $PSScriptRoot '../../../test/Test-UcalApiClientDeployment.ps1') -Api Vxl @PSBoundParameters
