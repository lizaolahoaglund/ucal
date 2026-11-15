[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}$')]
    [string]$Version,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
& (Join-Path $PSScriptRoot 'Test-UcalDistributionLicense.ps1') -PackageRoot $resolvedRoot
$sourceInfo=Get-Content -LiteralPath (Join-Path $resolvedRoot 'SOURCE.txt') -Raw -Encoding UTF8
$edition=if($sourceInfo -match '(?m)^Edition: (community|internal)\r?$'){$Matches[1]}else{'community'}
& (Join-Path $PSScriptRoot 'Test-UcalReleaseAssets.ps1') -PackageRoot $resolvedRoot -Edition $edition
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "Package root is not a directory: $PackageRoot"
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $resolvedRoot 'UcalManagerPackage.json'
}
$fullOutput = [IO.Path]::GetFullPath($OutputPath)
$rootPrefix = $resolvedRoot.TrimEnd('\') + '\'
if (-not $fullOutput.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The package manifest must be written inside the package root.'
}

$requiredFiles = @(
    'UcalManager.exe',
    'Qt5Core.dll',
    'Qt5Gui.dll',
    'Qt5Network.dll',
    'Qt5Widgets.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll',
    'platforms/qwindows.dll',
    'runtime/x64/UcalJ2534DiscoveryHost.exe',
    'runtime/x86/UcalJ2534DiscoveryHost.exe',
    'runtime/x64/UcalBroker.exe',
    'runtime/x86/UcalBroker.exe',
    'runtime/x64/UcalBackendJ2534.dll',
    'runtime/x86/UcalBackendJ2534.dll'
)
foreach ($kind in @('backends','frontends')) {
    $folder = Join-Path $resolvedRoot $kind
    if (-not (Test-Path -LiteralPath $folder)) { continue }
    foreach ($metadata in Get-ChildItem -LiteralPath $folder -Filter '*.json' -Recurse -File) {
        if ($metadata.Name -notin @('backend.json','frontend.json')) { continue }
        $package = Get-Content -LiteralPath $metadata.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $package.library) { throw "Package does not declare its DLL: $($metadata.FullName)" }
        $library = ([string]$package.library).Replace('{architecture}','x64')
        $target = [IO.Path]::GetFullPath((Join-Path $metadata.DirectoryName $library))
        if (-not $target.StartsWith($resolvedRoot.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Package DLL is missing or outside package: $target" }
    }
}

$forbiddenNames = @()
$allowedUcalCompatibilityPaths = @()
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
foreach ($category in @('frontends','backends')) {
    foreach ($descriptor in @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $category) -Filter plugin-package.json -Recurse -File)) {
        $module = Get-Content -LiteralPath $descriptor.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($module.PSObject.Properties['vendor_runtime_names']) { $forbiddenNames += @($module.vendor_runtime_names) }
        foreach ($asset in @($module.runtime_files)) {
            $allowedUcalCompatibilityPaths += ([string]$asset.destination).ToLowerInvariant()
        }
    }
}
$forbiddenExtensions = @('.pdb', '.log', '.dmp', '.png', '.jpg', '.jpeg', '.bmp')

$files = @()
foreach ($file in Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File) {
    if ($file.FullName -eq $fullOutput) { continue }
    $relative = $file.FullName.Substring($rootPrefix.Length).Replace('\', '/')
    if ($forbiddenNames -contains $file.Name.ToLowerInvariant() -and
        $relative.ToLowerInvariant() -notin $allowedUcalCompatibilityPaths) {
        throw "Vendor runtime is not allowed in the UCAL Manager package: $relative"
    }
    if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        throw "Generated/private evidence is not allowed in the package: $($file.Name)"
    }
    if ($file.Name.Equals('libgcc_s_dw2-1.dll', [StringComparison]::OrdinalIgnoreCase)) {
        throw "The 32-bit MinGW DW2 runtime is not allowed in the x64 package: $relative"
    }
    if ($relative -match '(^|/)(profiles|workspace)([-.]|$)' -or
        $relative -match '(^|/)(logs?|screenshots?)(/|$)') {
        throw "User data or review evidence is not allowed in the package: $relative"
    }
    $files += [ordered]@{
        path = $relative
        length = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}
$files = @($files | Sort-Object { $_.path })
$present = @($files | ForEach-Object { [string]$_.path })
foreach ($required in $requiredFiles) {
    if ($present -notcontains $required) {
        throw "Required package file is missing: $required"
    }
}

$manifest = [ordered]@{
    schema = 'org.ucal.manager-portable-package/2'
    product = 'UCAL Manager'
    version = $Version
    architecture = 'x64'
    maturity = 'engineering'
    distribution = 'portable-zip'
    requiresInstallation = $false
    entrypoint = 'UcalManager.exe'
    runtimeEntrypoint = 'runtime/x64/UcalBroker.exe'
    audience = 'end-user'
    edition = $edition
    developerToolsIncluded = $false
    pcanViewRuntime = 'package-defined adapters'
    pcanViewPythonRuntimeRequired = $false
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    fileCount = $files.Count
    files = $files
    extraction = [ordered]@{
        strategy = 'extract-to-versioned-directory'
        preserveUserDataOutsidePackage = $true
        exactHashVerification = $true
        removePackageDirectoryToRollback = $true
    }
}
[IO.File]::WriteAllText(
    $fullOutput,
    ($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 8 -Compress
