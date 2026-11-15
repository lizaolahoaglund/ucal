[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out')).TrimEnd('\')
$resolvedPackage = (Resolve-Path -LiteralPath $PackageRoot).Path.TrimEnd('\')
$manifestPath = Join-Path $resolvedPackage 'UcalManagerPackage.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Generate and verify the portable-package manifest before creating the ZIP.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]$manifest.schema -notin @('org.ucal.manager-portable-package/1','org.ucal.manager-portable-package/2') -or
    [string]$manifest.distribution -ne 'portable-zip' -or
    [bool]$manifest.requiresInstallation) {
    throw 'The package manifest is not a no-install portable ZIP contract.'
}

$fullOutput = [IO.Path]::GetFullPath($OutputPath)
if (-not $fullOutput.StartsWith($outRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetExtension($fullOutput) -ine '.zip') {
    throw 'Portable ZIP output must be a .zip file below the repository out directory.'
}
if ($fullOutput.StartsWith($resolvedPackage + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Portable ZIP output must not be inside the package payload.'
}
if (Test-Path -LiteralPath $fullOutput) {
    throw "Portable ZIP output already exists: $fullOutput"
}
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fullOutput)) | Out-Null

Compress-Archive -Path (Join-Path $resolvedPackage '*') `
    -DestinationPath $fullOutput -CompressionLevel Optimal

[ordered]@{
    schema = 'org.ucal.manager-portable-zip-result/1'
    status = 'created'
    packageVersion = [string]$manifest.version
    zipPath = $fullOutput
    length = (Get-Item -LiteralPath $fullOutput).Length
    sha256 = (Get-FileHash -LiteralPath $fullOutput -Algorithm SHA256).Hash
    requiresInstallation = $false
} | ConvertTo-Json -Depth 4
