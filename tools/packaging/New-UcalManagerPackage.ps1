[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [Alias('SwitchDeploymentRoot')][string]$ManagerDeploymentRoot,

    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [string]$RuntimeX64Root = 'out\x64\Release',
    [string]$RuntimeX86Root = 'out\Win32\Release',
    [string]$PcanViewNativeRoot = '',

    [Parameter(Mandatory = $true)]
    [string]$FrontendPluginRoot,
    [Parameter(Mandatory = $true)]
    [string]$RuntimeNoticesRoot,
    # Source exports without Git supply the matching archive and revision explicitly.
    [string]$SourceArchivePath,
    [ValidatePattern('^[0-9a-f]{40}$')][string]$SourceRevision,
    [string]$CorrespondingSourceDirectory,
    [switch]$IncludePrivate,
    [switch]$PublicOnly
)

$ErrorActionPreference = 'Stop'
if($IncludePrivate -and $PublicOnly){throw 'Select either the community edition or the internal edition.'}
$edition=if($IncludePrivate){'internal'}else{'community'}
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out')).TrimEnd('\')

function Resolve-UcalPackageInput([string]$Path, [string]$Label, [bool]$Directory) {
    $candidate = if ([IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $repoRoot $Path }
    if (-not (Test-Path -LiteralPath $candidate -PathType $(if ($Directory) { 'Container' } else { 'Leaf' }))) {
        throw "$Label was not found: $candidate"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

$managerRoot = Resolve-UcalPackageInput $ManagerDeploymentRoot 'Qt deployment root' $true
$runtimeX64 = Resolve-UcalPackageInput $RuntimeX64Root 'x64 UCAL runtime root' $true
$runtimeX86 = Resolve-UcalPackageInput $RuntimeX86Root 'x86 UCAL runtime root' $true
$pcanViewNative = if ([string]::IsNullOrWhiteSpace($PcanViewNativeRoot)) {
    $runtimeX64
} else {
    Resolve-UcalPackageInput $PcanViewNativeRoot 'native PCAN-View runtime root' $true
}
$destination = [IO.Path]::GetFullPath(
    $(if ([IO.Path]::IsPathRooted($PackageRoot)) { $PackageRoot } else { Join-Path $repoRoot $PackageRoot }))
$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist')).TrimEnd('\')
if (-not ($destination.StartsWith($outRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
          $destination.Equals($distRoot, [StringComparison]::OrdinalIgnoreCase) -or
          $destination.StartsWith($distRoot + '\', [StringComparison]::OrdinalIgnoreCase))) {
    throw 'Portable-package assembly is restricted to dist or a new directory below out.'
}
if (Test-Path -LiteralPath $destination) {
    if (@(Get-ChildItem -LiteralPath $destination -Force | Where-Object { $_.Name -ne 'README.md' -or -not $destination.Equals($distRoot, [StringComparison]::OrdinalIgnoreCase) }).Count -ne 0) {
        throw "Package destination is not empty: $destination"
    }
} else {
    New-Item -ItemType Directory -Path $destination | Out-Null
}

function Copy-UcalPackageFile([string]$Source, [string]$RelativeDestination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required package input was not found: $Source"
    }
    $target = Join-Path $destination $RelativeDestination
    $targetDirectory = [IO.Path]::GetDirectoryName($target)
    if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $target -Force
}

foreach($relative in @('LICENSE','LICENSE_SCOPE.md','THIRD_PARTY_NOTICES.md',
    'licenses/GPL-3.0.txt','licenses/Independent-modules-exception.txt','licenses/CompatibilitySupport.txt')){
    Copy-UcalPackageFile (Join-Path $repoRoot $relative) $relative
}
Copy-UcalPackageFile (Join-Path $repoRoot 'manager/doc/ReleaseReadme.md') 'README.md'
$editionNote='Package edition: '+$edition+'.'
Add-Content -LiteralPath (Join-Path $destination 'README.md') -Encoding UTF8 -Value ("`n"+$editionNote)
$noticeRoot=Resolve-UcalPackageInput $RuntimeNoticesRoot 'selected runtime notices' $true
$receipt=Get-Content -LiteralPath (Join-Path $noticeRoot 'RuntimeNotices.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($receipt.schema -ne 'org.ucal.runtime-notices/2' -or
    $receipt.profile -ne 'qt-5.14.2-mingw73-x64-windows' -or
    @($receipt.files).Count -ne 1 -or $receipt.files[0].path -ne 'ThirdPartyNotices.txt'){
    throw 'Generate the maintained Windows runtime notices before packaging.'
}
$noticeText=Join-Path $noticeRoot 'ThirdPartyNotices.txt'
if((Get-FileHash -LiteralPath $noticeText -Algorithm SHA256).Hash -ne $receipt.files[0].sha256){
    throw 'Runtime notice integrity mismatch in build inputs.'
}
# The receipt and upstream attribution JSON are build inputs, not user assets.
Copy-UcalPackageFile $noticeText 'licenses/ThirdPartyNotices.txt'
if(-not $SourceArchivePath){
    $gitRoot=(& git -C $repoRoot rev-parse --show-toplevel).Trim()
    if($LASTEXITCODE -ne 0 -or -not [IO.Path]::GetFullPath($gitRoot).Equals($repoRoot,[StringComparison]::OrdinalIgnoreCase)){
        throw 'Source exports must supply -SourceArchivePath and -SourceRevision.'
    }
    & git -C $repoRoot diff --quiet HEAD --
    if($LASTEXITCODE -ne 0){throw 'Commit the covered source before assembling an exact-source binary package.'}
    $SourceRevision=(& git -C $repoRoot rev-parse HEAD).Trim()
    if($LASTEXITCODE -ne 0){throw 'Source exports must supply -SourceArchivePath and -SourceRevision.'}
    $sourceRoot=Join-Path $outRoot ('package-source-'+[guid]::NewGuid().ToString('N'))
    $SourceArchivePath=Join-Path $outRoot ('package-source-'+[guid]::NewGuid().ToString('N')+'.tar')
    & python (Join-Path $PSScriptRoot 'Export-UcalSource.py') --root $repoRoot --output $sourceRoot --revision $SourceRevision --archive-output $SourceArchivePath
    if($LASTEXITCODE -ne 0){throw 'Public corresponding-source export failed.'}
}
if(-not $SourceRevision){throw 'The source archive needs its exact -SourceRevision.'}
$sourceArchive=Resolve-UcalPackageInput $SourceArchivePath 'exact public corresponding source' $false
if(-not $CorrespondingSourceDirectory){$CorrespondingSourceDirectory=Join-Path $outRoot 'corresponding-source'}
$sourceDestination=[IO.Path]::GetFullPath($CorrespondingSourceDirectory)
if(-not $sourceDestination.StartsWith($outRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Corresponding-source artifacts must remain below ignored out.'}
New-Item -ItemType Directory -Force -Path $sourceDestination | Out-Null
$sourceName='UcalManager-source-'+$SourceRevision.Substring(0,12)+'.tar'
$sourceTarget=Join-Path $sourceDestination $sourceName
if(Test-Path -LiteralPath $sourceTarget){
    if((Get-FileHash -LiteralPath $sourceTarget).Hash -ne (Get-FileHash -LiteralPath $sourceArchive).Hash){throw 'Existing corresponding-source artifact differs; choose a new directory.'}
}else{Copy-Item -LiteralPath $sourceArchive -Destination $sourceTarget}
$sourceHash=(Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash
$sourceInfo=@"
UCAL corresponding source

Revision: $SourceRevision
Edition: $edition
Archive: $sourceName
SHA256: $sourceHash
License: LGPL-3.0-only WITH Independent-modules-exception

Download the matching source archive attachment from the same release as this ZIP.
Build instructions: README.md, manager/doc/README.md and test/Running.md in that archive.
Independent proprietary compatibility support implementations are not included.
Dependency information: THIRD_PARTY_NOTICES.md and licenses/ThirdPartyNotices.txt.
"@
[IO.File]::WriteAllText((Join-Path $destination 'SOURCE.txt'),$sourceInfo,[Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot 'Test-UcalDistributionLicense.ps1') -PackageRoot $destination -SourceArchivePath $sourceTarget

foreach ($file in Get-ChildItem -LiteralPath $managerRoot -Recurse -File) {
    $relative = $file.FullName.Substring($managerRoot.TrimEnd('\').Length + 1)
    Copy-UcalPackageFile $file.FullName $relative
}

$runtimeFiles = @(
    @{ Root = $runtimeX64; Source = 'UcalBroker.exe'; Destination = 'runtime\x64\UcalBroker.exe' },
    @{ Root = $runtimeX86; Source = 'UcalBroker.exe'; Destination = 'runtime\x86\UcalBroker.exe' },
    @{ Root = $runtimeX64; Source = 'UcalJ2534DiscoveryHost.exe'; Destination = 'runtime\x64\UcalJ2534DiscoveryHost.exe' },
    @{ Root = $runtimeX86; Source = 'UcalJ2534DiscoveryHost.exe'; Destination = 'runtime\x86\UcalJ2534DiscoveryHost.exe' },
    @{ Root = $runtimeX64; Source = 'UcalBackendJ2534.dll'; Destination = 'runtime\x64\UcalBackendJ2534.dll' },
    @{ Root = $runtimeX86; Source = 'UcalBackendJ2534.dll'; Destination = 'runtime\x86\UcalBackendJ2534.dll' }
)
foreach ($entry in $runtimeFiles) {
    Copy-UcalPackageFile (Join-Path $entry.Root $entry.Source) $entry.Destination
}

$runtimeRoots = @{ x64 = $runtimeX64; x86 = $runtimeX86; native = $pcanViewNative }
$includedBackendIds = @()
$includedSoftwareIds = @()
$privateModuleCount=0
$sourceRoots = @('frontends','backends') | ForEach-Object { Join-Path $repoRoot $_ } | Where-Object { Test-Path -LiteralPath $_ }
foreach ($descriptor in @($sourceRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter 'plugin-package.json' -Recurse -File } | Sort-Object FullName)) {
    $plugin = Get-Content -LiteralPath $descriptor.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if (($PublicOnly -or -not $IncludePrivate) -and $plugin.visibility -eq 'private') { continue }
    if($plugin.visibility -eq 'private'){$privateModuleCount++}
    $softwareMetadata=Join-Path $descriptor.DirectoryName 'frontend.json'
    if(Test-Path -LiteralPath $softwareMetadata){
        $includedSoftwareIds+=(Get-Content -LiteralPath $softwareMetadata -Raw -Encoding UTF8 | ConvertFrom-Json).id
    }
    foreach ($metadataName in @($plugin.backends)) {
        $metadata = Get-Content -LiteralPath (Join-Path $descriptor.DirectoryName $metadataName) -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($metadata.type -eq 'ucal') { $includedBackendIds += ([string]$metadata.id).ToLowerInvariant() }
    }
    if ($plugin.schema -notin @('org.ucal.plugin-source-package/1','org.ucal.plugin-source-package/2')) { throw "Unsupported source package: $($descriptor.FullName)" }
    if ($plugin.schema -eq 'org.ucal.plugin-source-package/1') {
        $plugin | Add-Member -NotePropertyName runtime_files -NotePropertyValue @($plugin.runtimeFiles) -Force
        $plugin | Add-Member -NotePropertyName source_files -NotePropertyValue @($plugin.sourceFiles) -Force
    }
    foreach ($asset in $plugin.runtime_files) {
        if (-not $runtimeRoots.ContainsKey($asset.root)) { throw 'Unknown runtime architecture.' }
        Copy-UcalPackageFile (Join-Path $runtimeRoots[$asset.root] $asset.source) $asset.destination
    }
    foreach ($relative in $plugin.backends) {
        $metadata = Join-Path $descriptor.DirectoryName $relative
        $backend = Get-Content -LiteralPath $metadata -Raw -Encoding UTF8 | ConvertFrom-Json
        $backend.PSObject.Properties.Remove('configuration')
        $metadataTarget=Join-Path $destination "backends/$($backend.id)/backend.json"
        New-Item -ItemType Directory -Force -Path (Split-Path $metadataTarget -Parent) | Out-Null
        [IO.File]::WriteAllText($metadataTarget,($backend | ConvertTo-Json -Depth 16),[Text.UTF8Encoding]::new($false))
        foreach ($architecture in @('x64','x86')) {
            $name = [IO.Path]::GetFileName($backend.library)
            Copy-UcalPackageFile (Join-Path $runtimeRoots[$architecture] $name) "backends/$($backend.id)/$architecture/$name"
        }
    }
}
if($IncludePrivate -and $privateModuleCount -eq 0){throw 'The internal edition requires the private source groups.'}
foreach ($architecture in @('x64','x86')) {
    $lines = @()
    $section = ''
    foreach ($line in Get-Content -LiteralPath (Join-Path $runtimeRoots[$architecture] 'ucal_native_backends.ini')) {
        if ($line -match '^\[([^]]+)\]$') { $section = $Matches[1]; $lines += $line; continue }
        if ($line -notmatch '^([^=]+)=(.*)$') { if (-not $line.Trim()) { $lines += $line }; continue }
        $key = $Matches[1].ToLowerInvariant()
        $value = $Matches[2].ToLowerInvariant()
        $id = if ($section -eq 'aliases') { $value } else { $key }
        if ($id -in $includedBackendIds) { $lines += $line }
    }
    $target = Join-Path $destination "runtime/$architecture/ucal_native_backends.ini"
    [IO.File]::WriteAllLines($target, [string[]]$lines, [Text.UTF8Encoding]::new($false))
}
$frontendPackages = Resolve-UcalPackageInput $FrontendPluginRoot 'frontend DLL package root' $true
foreach ($file in Get-ChildItem -LiteralPath $frontendPackages -Recurse -File) {
    $relative = $file.FullName.Substring($frontendPackages.TrimEnd('\').Length + 1)
    # Ignore stale build outputs from another edition without deleting caches.
    $softwareId=$relative.Replace('\','/').Split('/')[0]
    if($softwareId -notin $includedSoftwareIds){continue}
    if($file.Name -eq 'frontend.json'){
        $metadata=Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $metadata.PSObject.Properties.Remove('launch')
        $target=Join-Path $destination (Join-Path 'frontends' $relative)
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        [IO.File]::WriteAllText($target,($metadata | ConvertTo-Json -Depth 16),[Text.UTF8Encoding]::new($false))
    }else{Copy-UcalPackageFile $file.FullName (Join-Path 'frontends' $relative)}
}

[ordered]@{
    schema = 'org.ucal.manager-package-assembly/1'
    status = 'assembled'
    packageRoot = $destination
    edition = $edition
    correspondingSource = $sourceTarget
    fileCount = @(Get-ChildItem -LiteralPath $destination -Recurse -File).Count
    vendorRuntimesIncluded = $false
    next = 'Generate the manifest, run functional package validation, then create the ZIP.'
} | ConvertTo-Json -Depth 4
