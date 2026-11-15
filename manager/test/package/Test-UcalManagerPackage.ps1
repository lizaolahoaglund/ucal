[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [string]$ManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$ExtractionRoot,

    [string]$OutputPath,
    # Test tooling belongs to the source checkout, never the user package.
    [string]$ProbePath,

    [switch]$SkipFunctionalSmoke
)

$ErrorActionPreference = 'Stop'
$resolvedPackage = (Resolve-Path -LiteralPath $PackageRoot).Path
& (Join-Path $PSScriptRoot '../../../tools/packaging/Test-UcalDistributionLicense.ps1') -PackageRoot $resolvedPackage
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $resolvedPackage 'UcalManagerPackage.json'
}
$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$fullExtractionRoot = [IO.Path]::GetFullPath($ExtractionRoot).TrimEnd('\')
$packagePrefix = $resolvedPackage.TrimEnd('\') + '\'
if (-not $resolvedManifest.StartsWith($packagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Manifest must be inside the package root.'
}
if ($fullExtractionRoot.Equals($resolvedPackage, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Extraction root must not be the package source directory.'
}

$protectedRoots = @(
    [Environment]::GetFolderPath('Windows'),
    [Environment]::GetFolderPath('System'),
    [Environment]::GetFolderPath('ProgramFiles'),
    [Environment]::GetFolderPath('ProgramFilesX86')
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
foreach ($protected in $protectedRoots) {
    $fullProtected = [IO.Path]::GetFullPath($protected).TrimEnd('\')
    if ($fullExtractionRoot.Equals($fullProtected, [StringComparison]::OrdinalIgnoreCase) -or
        $fullExtractionRoot.StartsWith($fullProtected + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Portable-package validation refuses protected or vendor trees: $fullExtractionRoot"
    }
}

$manifest = Get-Content -LiteralPath $resolvedManifest -Raw -Encoding UTF8 | ConvertFrom-Json
$errors = [Collections.Generic.List[string]]::new()
$sourceInfo=Get-Content -LiteralPath (Join-Path $resolvedPackage 'SOURCE.txt') -Raw -Encoding UTF8
$edition=if($sourceInfo -match '(?m)^Edition: (community|internal)\r?$'){$Matches[1]}else{'community'}
$manifestEdition=if($manifest.PSObject.Properties['edition']){[string]$manifest.edition}else{'community'}
if($manifestEdition -ne $edition){$errors.Add('Package edition does not match its source information.')}
$requiredFiles = @(
    'UcalManager.exe',
    'Qt5Network.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll',
    'platforms\qwindows.dll',
    'runtime/x64/UcalJ2534DiscoveryHost.exe',
    'runtime/x86/UcalJ2534DiscoveryHost.exe',
    'runtime\x64\UcalBroker.exe',
    'runtime\x86\UcalBroker.exe',
    'runtime/x64/UcalBackendJ2534.dll',
    'runtime/x86/UcalBackendJ2534.dll'
)
foreach ($kind in @('backends','frontends')) {
    $folder = Join-Path $resolvedPackage $kind
    if (-not (Test-Path -LiteralPath $folder)) { continue }
    foreach ($metadata in Get-ChildItem -LiteralPath $folder -Filter '*.json' -Recurse -File) {
        if ($metadata.Name -notin @('backend.json','frontend.json')) { continue }
        $package = Get-Content -LiteralPath $metadata.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $package.library) { throw "Package does not declare its DLL: $($metadata.FullName)" }
        $library = ([string]$package.library).Replace('{architecture}','x64')
        $target = [IO.Path]::GetFullPath((Join-Path $metadata.DirectoryName $library))
        if (-not $target.StartsWith($resolvedPackage.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Package DLL is missing or outside package: $target" }
    }
}

if ([string]$manifest.schema -ne 'org.ucal.manager-portable-package/2') {
    $errors.Add('Unsupported package manifest schema.')
}
if ([string]$manifest.entrypoint -ne 'UcalManager.exe') {
    $errors.Add('Unexpected package entrypoint.')
}
if ([string]$manifest.runtimeEntrypoint -ne 'runtime/x64/UcalBroker.exe' -or
    [string]$manifest.audience -ne 'end-user' -or [bool]$manifest.developerToolsIncluded) {
    $errors.Add('Portable package does not declare the UCAL runtime smoke entrypoints.')
}
if ([string]$manifest.architecture -ne 'x64') {
    $errors.Add('Unexpected package architecture.')
}
if ([string]$manifest.distribution -ne 'portable-zip' -or
    [bool]$manifest.requiresInstallation) {
    $errors.Add('Package is not declared as a no-install portable ZIP payload.')
}
if ([string]$manifest.pcanViewRuntime -ne 'package-defined adapters' -or
    [bool]$manifest.pcanViewPythonRuntimeRequired) {
    $errors.Add('Portable package does not declare the native no-Python PCAN-View runtime.')
}

$manifestFiles = @{}
foreach ($entry in @($manifest.files)) {
    $relative = ([string]$entry.path).Replace('/', '\')
    if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
        $relative.Split('\') -contains '..') {
        $errors.Add("Unsafe manifest path: $relative")
        continue
    }
    if ($manifestFiles.ContainsKey($relative)) {
        $errors.Add("Duplicate manifest path: $relative")
        continue
    }
    $candidate = [IO.Path]::GetFullPath((Join-Path $resolvedPackage $relative))
    if (-not $candidate.StartsWith($packagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $errors.Add("Manifest path escapes package root: $relative")
        continue
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $errors.Add("Manifest file is missing: $relative")
        continue
    }
    $file = Get-Item -LiteralPath $candidate
    $actualHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
    if ([int64]$entry.length -ne [int64]$file.Length -or
        [string]$entry.sha256 -ne $actualHash) {
        $errors.Add("Manifest integrity mismatch: $relative")
        continue
    }
    $manifestFiles[$relative] = $actualHash
}

$extraFiles = @()
foreach ($file in Get-ChildItem -LiteralPath $resolvedPackage -Recurse -File) {
    if ($file.FullName -eq $resolvedManifest) { continue }
    $relative = $file.FullName.Substring($packagePrefix.Length)
    if (-not $manifestFiles.ContainsKey($relative)) { $extraFiles += $relative.Replace('\', '/') }
}
if ($extraFiles.Count -gt 0) {
    $errors.Add('Unmanifested package files: ' + ($extraFiles -join ', '))
}
if ([int]$manifest.fileCount -ne $manifestFiles.Count) {
    $errors.Add('Manifest fileCount does not match the verified file set.')
}
foreach ($required in $requiredFiles) {
    if (-not $manifestFiles.ContainsKey($required.Replace('/', '\'))) {
        $errors.Add("Required functional package file is absent from the manifest: $required")
    }
}

if($errors.Count -eq 0){
    try{ & (Join-Path $PSScriptRoot '../../../tools/packaging/Test-UcalReleaseAssets.ps1') -PackageRoot $resolvedPackage -Edition $edition }
    catch{ $errors.Add($_.Exception.Message) }
}
$entrypointTarget = Join-Path $fullExtractionRoot 'UcalManager.exe'
$runningTargetPids = @()
foreach ($process in Get-Process -Name 'UcalManager' -ErrorAction SilentlyContinue) {
    try {
        if ($process.Path -and
            [IO.Path]::GetFullPath($process.Path).Equals(
                $entrypointTarget, [StringComparison]::OrdinalIgnoreCase)) {
            $runningTargetPids += $process.Id
        }
    } catch {}
}
if ($runningTargetPids.Count -gt 0) {
    $errors.Add('The target UCAL Manager entrypoint is running.')
}

$changes = @()
foreach ($entry in @($manifest.files)) {
    $relative = ([string]$entry.path).Replace('/', '\')
    $target = Join-Path $fullExtractionRoot $relative
    $action = 'create'
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $action = if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq
            [string]$entry.sha256) { 'retain' } else { 'replace' }
    }
    $changes += [ordered]@{ path = $relative.Replace('\', '/'); action = $action }
}

$functionalSmoke = [ordered]@{
    status = if ($SkipFunctionalSmoke) { 'skipped' } else { 'not-run' }
    switchLaunch = $false
    switchRender = $false
    switchCleanExit = $false
    ping = $false
    enumerate = $false
    cleanShutdown = $false
    error = $null
}
if ($errors.Count -eq 0 -and -not $SkipFunctionalSmoke) {
    $switchPath = Join-Path $resolvedPackage 'UcalManager.exe'
    $brokerPath = Join-Path $resolvedPackage 'runtime\x64\UcalBroker.exe'
    if(-not $ProbePath){$ProbePath=Join-Path $PSScriptRoot '../../../out/x64/Release/UcalHarness.exe'}
    $harnessPath=(Resolve-Path -LiteralPath $ProbePath).Path
    $switchProcess = $null
    $brokerProcess = $null
    $switchWorkspace = [IO.Path]::GetTempFileName()
    $switchScreenshot = Join-Path ([IO.Path]::GetTempPath()) `
        ("UcalManager-package-" + [Guid]::NewGuid().ToString('N') + '.png')
    $stdoutPath = [IO.Path]::GetTempFileName()
    $stderrPath = [IO.Path]::GetTempFileName()
    $savedBackend = [Environment]::GetEnvironmentVariable('UCAL_BACKEND', 'Process')
    $savedInstance = [Environment]::GetEnvironmentVariable('UCAL_BROKER_INSTANCE', 'Process')
    $savedSettings = [Environment]::GetEnvironmentVariable('UCAL_MANAGER_SETTINGS_DIRECTORY', 'Process')
    $smokeSettings = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('ucal_package_settings_' + [Guid]::NewGuid().ToString('N'))))
    $temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$smokeSettings.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid smoke settings directory.' }
    try {
        [Environment]::SetEnvironmentVariable('UCAL_BROKER_INSTANCE', 'package-smoke-' + [Guid]::NewGuid().ToString('N'), 'Process')
        [Environment]::SetEnvironmentVariable('UCAL_MANAGER_SETTINGS_DIRECTORY', $smokeSettings, 'Process')
        # Own the test service before opening the live UI. Otherwise Switch may
        # correctly auto-start a hardware Broker which this smoke cannot own.
        [Environment]::SetEnvironmentVariable('UCAL_BACKEND', $null, 'Process')
        $brokerProcess = Start-Process -FilePath $brokerPath -ArgumentList '--allow-test-shutdown' `
            -WorkingDirectory $resolvedPackage -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        for ($attempt = 0; $attempt -lt 20; ++$attempt) {
            & $harnessPath ping *> $null
            if ($LASTEXITCODE -eq 0) { $functionalSmoke.ping = $true; break }
            Start-Sleep -Milliseconds 100
        }
        if (-not $functionalSmoke.ping) { throw 'Packaged Broker did not answer ping.' }
        Remove-Item -LiteralPath $switchWorkspace -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $switchScreenshot -Force -ErrorAction SilentlyContinue
        $switchArguments = @(
            '--presentation', 'production', '--language', 'en', '--live',
            '--workspace', ('"' + $switchWorkspace + '"'), '--page', 'settings',
            '--screenshot', ('"' + $switchScreenshot + '"'))
        $switchProcess = Start-Process -FilePath $switchPath -ArgumentList $switchArguments `
            -WorkingDirectory $resolvedPackage -PassThru
        $functionalSmoke.switchLaunch = $true
        if (-not $switchProcess.WaitForExit(15000)) {
            throw 'Packaged UCAL Manager did not finish its render smoke in time.'
        }
        if ($switchProcess.ExitCode -ne 0) {
            throw "Packaged UCAL Manager exited with code $($switchProcess.ExitCode)."
        }
        $functionalSmoke.switchCleanExit = $true
        if (-not (Test-Path -LiteralPath $switchScreenshot -PathType Leaf) -or
            (Get-Item -LiteralPath $switchScreenshot).Length -eq 0) {
            throw 'Packaged UCAL Manager did not render its smoke screenshot.'
        }
        $functionalSmoke.switchRender = $true

        & $harnessPath enumerate *> $null
        if ($LASTEXITCODE -ne 0) { throw 'Source-built endpoint probe could not enumerate the packaged Broker.' }
        $functionalSmoke.enumerate = $true
        & $harnessPath shutdown *> $null
        if ($LASTEXITCODE -ne 0) { throw 'Packaged Broker rejected owned smoke shutdown.' }
        if (-not $brokerProcess.WaitForExit(5000)) { throw 'Packaged Broker did not stop in time.' }
        $functionalSmoke.cleanShutdown = $true
        $functionalSmoke.status = 'passed'
    }
    catch {
        $functionalSmoke.status = 'failed'
        $functionalSmoke.error = $_.Exception.Message
        $errors.Add("Functional package smoke failed: $($functionalSmoke.error)")
    }
    finally {
        [Environment]::SetEnvironmentVariable('UCAL_BACKEND', $savedBackend, 'Process')
        [Environment]::SetEnvironmentVariable('UCAL_BROKER_INSTANCE', $savedInstance, 'Process')
        [Environment]::SetEnvironmentVariable('UCAL_MANAGER_SETTINGS_DIRECTORY', $savedSettings, 'Process')
        if ($switchProcess) {
            $switchProcess.Refresh()
            if (-not $switchProcess.HasExited) {
                Stop-Process -Id $switchProcess.Id -Force -ErrorAction SilentlyContinue
                $null = $switchProcess.WaitForExit(5000)
            }
            $switchProcess.Dispose()
        }
        if ($brokerProcess) {
            $brokerProcess.Refresh()
            if (-not $brokerProcess.HasExited) {
                Stop-Process -Id $brokerProcess.Id -Force -ErrorAction SilentlyContinue
                $null = $brokerProcess.WaitForExit(5000)
            }
            $brokerProcess.Dispose()
        }
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $switchWorkspace -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $switchScreenshot -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $smokeSettings -PathType Container) {
            Remove-Item -LiteralPath $smokeSettings -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
$functionalReady = $functionalSmoke.status -eq 'passed'

$result = [ordered]@{
    schema = 'org.ucal.manager-portable-validation/1'
    status = if ($errors.Count -ne 0) { 'blocked' }
             elseif ($functionalReady) { 'passed' }
             else { 'integrity-only' }
    packageVersion = [string]$manifest.version
    packageRoot = $resolvedPackage
    extractionRoot = $fullExtractionRoot
    verifiedFileCount = $manifestFiles.Count
    integrityReady = $errors.Count -eq 0
    portableReady = $errors.Count -eq 0 -and $functionalReady
    blockers = @($errors)
    functionalSmoke = $functionalSmoke
    changes = $changes
    extraction = [ordered]@{
        preconditions = @(
            'Extract into a user-selected, non-protected directory.',
            'Verify every manifest hash after extraction.',
            'Preserve the portable data folder when replacing or rolling back program files.',
            'Stop only the exact portable entrypoint before replacing its directory.'
        )
        targetDirectory = $fullExtractionRoot
        launch = 'run UcalManager.exe directly; no installer or registration required'
        rollback = 'remove this version directory and use the previous extracted version'
    }
}
$json = $result | ConvertTo-Json -Depth 8
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $fullOutput = [IO.Path]::GetFullPath($OutputPath)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fullOutput)) | Out-Null
    [IO.File]::WriteAllText($fullOutput, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}
$json
if ($errors.Count -ne 0) { exit 1 }
