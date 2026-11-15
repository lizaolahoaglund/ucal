[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('Win32','x64')][string]$Platform = 'x64',
    [string]$MSBuildPath = $env:UCAL_MSBUILD,
    [switch]$IncludeTests,
    [Alias('IncludeExtensions')][switch]$IncludePrivate,
    [switch]$PublicSourceOnly,
    [string]$NativeAdapterOutDirectory,
    [string]$NativeAdapterIntermediateDirectory
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'UcalToolchain.psm1') -Force
$msbuild = Get-UcalMSBuildPath $MSBuildPath
$roots = @('frontends','backends') | ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path -LiteralPath $_ }
$groups = @($roots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Directory } | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'plugin-package.json') -PathType Leaf
} | Where-Object {
    $description = Get-Content -LiteralPath (Join-Path $_.FullName 'plugin-package.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $IncludePrivate -or $description.visibility -ne 'private'
})
$projects = @($groups | ForEach-Object {
    $moduleRoot = $_.FullName
    $module = Get-Content -LiteralPath (Join-Path $moduleRoot 'plugin-package.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $privatePatterns = @()
    if ($module.PSObject.Properties['private_sources']) { $privatePatterns = @($module.private_sources) }
    $moduleProjects = @(Get-ChildItem -LiteralPath $moduleRoot -Filter '*.vcxproj' -File)
    if ($IncludeTests) {
        $testDirectory = Join-Path $moduleRoot 'test'
        if (Test-Path -LiteralPath $testDirectory) {
            $moduleProjects += @(Get-ChildItem -LiteralPath $testDirectory -Filter '*.vcxproj' -Recurse -File)
        }
    }
    foreach ($moduleProject in $moduleProjects) {
        $relative = $moduleProject.FullName.Substring($moduleRoot.Length + 1).Replace('\','/')
        if ($PublicSourceOnly -and @($privatePatterns | Where-Object { $relative -like $_ }).Count) { continue }
        $moduleProject
    }
} | Sort-Object FullName)
foreach ($project in $projects) {
    [xml]$definition = Get-Content -LiteralPath $project.FullName -Raw
    $configurations = @($definition.SelectNodes("//*[local-name()='ProjectConfiguration']") | ForEach-Object { $_.Include })
    if ("$Configuration|$Platform" -notin $configurations) {
        Write-Host "Not applicable to $Platform`: $($project.Name)"
        continue
    }
    $types = @($definition.SelectNodes("//*[local-name()='ConfigurationType']") | ForEach-Object { $_.InnerText })
    if ($types -contains 'Makefile') {
        Write-Host "Manual tool with external input; build its project explicitly: $($project.Name)"
        continue
    }
    $arguments = @($project.FullName, '/m', '/nologo', '/verbosity:minimal',
        "/p:Configuration=$Configuration", "/p:Platform=$Platform", "/p:SolutionDir=$($repo.Replace('\','/'))/")
    if ($PublicSourceOnly) { $arguments += '/p:UcalBuildPrivateSupport=false' }
    if ($NativeAdapterOutDirectory) { $arguments += "/p:UcalNativeAdapterOutDir=$NativeAdapterOutDirectory/" }
    if ($NativeAdapterIntermediateDirectory) { $arguments += "/p:UcalNativeAdapterIntDir=$NativeAdapterIntermediateDirectory/" }
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "Plugin build failed: $($project.FullName)" }
}

# Legacy direct-backend selectors are plugin metadata. Normal product discovery
# uses complete backend packages and the versioned plugin API instead.
$metadataRoots = @($groups | ForEach-Object { $_.FullName })
$packages = @($metadataRoots | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Filter 'backend.json' -Recurse -File
} | Sort-Object FullName)
$aliases = @{}; $isolation = @{}; $libraries = @{}
foreach ($file in $packages) {
    $package = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $package.PSObject.Properties['broker_aliases']) { $package | Add-Member -NotePropertyName broker_aliases -NotePropertyValue @($package.brokerAliases) -Force }
    if ($package.type -ne 'ucal' -or -not $package.broker_aliases) { continue }
    if ($package.id -notmatch '^[a-z0-9-]+$') { throw "Invalid native backend ID: $($file.FullName)" }
    foreach ($alias in @($package.id) + @($package.broker_aliases)) {
        if ($alias -notmatch '^[a-zA-Z0-9-]+$' -or $aliases.ContainsKey($alias.ToLowerInvariant())) {
            if ($aliases[$alias.ToLowerInvariant()] -eq $package.id) { continue }
            throw "Invalid or duplicate native backend alias: $alias"
        }
        $aliases[$alias.ToLowerInvariant()] = $package.id
    }
    $isolation[$package.id] = [int][bool]$package.isolated
    $library = [IO.Path]::GetFileName([string]$package.library)
    if (-not $library -or $library -notmatch '^[A-Za-z0-9_.-]+\.dll$') {
        throw "Invalid native backend library: $($file.FullName)"
    }
    $libraries[$package.id] = $library
}
$lines = @('[aliases]')
foreach ($key in @($aliases.Keys | Sort-Object)) { $lines += "$key=$($aliases[$key])" }
$lines += '[isolated]'
foreach ($key in @($isolation.Keys | Sort-Object)) { $lines += "$key=$($isolation[$key])" }
$lines += '[libraries]'
foreach ($key in @($libraries.Keys | Sort-Object)) { $lines += "$key=$($libraries[$key])" }
$output = Join-Path $repo "out/$Platform/$Configuration"
New-Item -ItemType Directory -Force -Path $output | Out-Null
[IO.File]::WriteAllText((Join-Path $output 'ucal_native_backends.ini'), ($lines -join "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Output "Built $($projects.Count) optional plugin projects ($Platform/$Configuration)."
