[CmdletBinding()]
param(
    [ValidateRange(1,100)][int]$RestartIterations = 5,
    [string]$MSBuildPath = $env:UCAL_MSBUILD,
    [switch]$RequirePrivateSupport
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$privateSources = (Test-Path -LiteralPath (Join-Path $repo 'frontends/vxlapi/src/XlRuntime.cpp')) -and
    (Test-Path -LiteralPath (Join-Path $repo 'frontends/pcanview/src/bridge/BridgeAdapter.cpp'))
if ($RequirePrivateSupport -and -not $privateSources) { throw 'Private compatibility qualification sources are required for this run.' }
$arguments = @{RestartIterations=$RestartIterations;MSBuildPath=$MSBuildPath}
if ($privateSources) {
    & (Join-Path $repo 'test/integration/Test-UcalCompatibility.ps1') @arguments
} else {
    & (Join-Path $repo 'test/integration/Test-UcalPublicCore.ps1') @arguments
}
exit $LASTEXITCODE
