[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$QmakePath,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [ValidateSet('x86','x64')][string]$Architecture='x64',
    [string]$MakePath='nmake.exe',
    [switch]$IncludePrivate
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output=[IO.Path]::GetFullPath($OutputRoot)
$profiles=@(Get-ChildItem -LiteralPath (Join-Path $repo 'frontends') -Filter frontend.json -Recurse -File | Sort-Object FullName)
foreach($metadata in $profiles) {
    $descriptor=Join-Path $metadata.DirectoryName 'plugin-package.json'
    $module=Get-Content -LiteralPath $descriptor -Raw -Encoding UTF8 | ConvertFrom-Json
    if($module.visibility -eq 'private' -and -not $IncludePrivate){continue}
    $description=Get-Content -LiteralPath $metadata.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $family=$description.id
    $source=$metadata.DirectoryName
    $projects=@(Get-ChildItem -LiteralPath $source -Filter '*.pro' -File)
    if($projects.Count -ne 1){throw "Expected one frontend project beside $($metadata.FullName)"}
    $build=Join-Path $output "build-$family-$Architecture"
    New-Item -ItemType Directory -Force $build | Out-Null
    Push-Location $build
    try {
        & $QmakePath $projects[0].FullName 'CONFIG+=release' 'CONFIG-=debug'
        if($LASTEXITCODE -ne 0){throw "qmake failed: $family"}
        & $MakePath
        if($LASTEXITCODE -ne 0){throw "frontend build failed: $family"}
    } finally { Pop-Location }
    $package=Join-Path $output ('frontends/'+$family)
    $target=Join-Path $package $Architecture
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -LiteralPath $metadata.FullName -Destination $package -Force
    $library=[IO.Path]::GetFileName(([string]$description.library).Replace('{architecture}',$Architecture))
    Copy-Item -LiteralPath (Join-Path $build ('bin/'+$library)) -Destination $target -Force
}
Write-Output "Frontend packages: $(Join-Path $output 'frontends')"
