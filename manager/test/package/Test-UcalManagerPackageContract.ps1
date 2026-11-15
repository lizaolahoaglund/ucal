[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out')).TrimEnd('\')
$resolvedPackage = (Resolve-Path -LiteralPath $PackageRoot).Path
$testRoot = Join-Path $outRoot ("package-contract-" + [Guid]::NewGuid().ToString('N'))
$powershell = Join-Path $PSHOME 'powershell.exe'
$validator = Join-Path $PSScriptRoot 'Test-UcalManagerPackage.ps1'
$manifestGenerator = Join-Path $repoRoot 'tools/packaging/New-UcalManagerPackageManifest.ps1'
$results = [Collections.Generic.List[object]]::new()

function Invoke-UcalContractProcess([string]$Script, [string[]]$Arguments) {
    $savedErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments 2>&1 |
            Out-String
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorPreference
    }
    return [ordered]@{ ExitCode = $exitCode; Output = $output.Trim() }
}

function Copy-UcalContractFixture([string]$Name, [string]$OmitPath = '') {
    $destination = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $destination | Out-Null
    if ($OmitPath) {
        foreach ($file in Get-ChildItem -LiteralPath $resolvedPackage -Recurse -File) {
            $relative = $file.FullName.Substring($resolvedPackage.TrimEnd('\').Length+1).Replace('\','/')
            if ($relative -eq $OmitPath) { continue }
            $target = Join-Path $destination $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    } else {
        Get-ChildItem -LiteralPath $resolvedPackage -Force |
            Copy-Item -Destination $destination -Recurse -Force
    }
    return $destination
}

function Assert-UcalFailure([string]$Name, [System.Collections.IDictionary]$Invocation,
                            [string]$ExpectedText) {
    if ($Invocation.ExitCode -eq 0) { throw "$Name unexpectedly passed." }
    if ($Invocation.Output -notmatch [Regex]::Escape($ExpectedText)) {
        throw "$Name did not report '$ExpectedText'. Output: $($Invocation.Output)"
    }
    $results.Add([ordered]@{ name = $Name; status = 'passed'; expectedFailure = $ExpectedText })
}

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $baseline = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $resolvedPackage,
        '-ExtractionRoot', (Join-Path $testRoot 'baseline-extract'),
        '-SkipFunctionalSmoke')
    if ($baseline.ExitCode -ne 0 -or $baseline.Output -notmatch '"integrityReady"\s*:\s*true') {
        throw "Baseline package integrity validation failed: $($baseline.Output)"
    }
    $results.Add([ordered]@{ name = 'baseline-integrity'; status = 'passed' })

    $zipTool = Join-Path $repoRoot 'tools/packaging/New-UcalManagerPortableZip.ps1'
    $zipPath = Join-Path $testRoot 'portable.zip'
    $zipResult = Invoke-UcalContractProcess $zipTool @('-PackageRoot', $resolvedPackage, '-OutputPath', $zipPath)
    if ($zipResult.ExitCode -ne 0) { throw "Portable ZIP creation failed: $($zipResult.Output)" }

    $manifest = Get-Content -LiteralPath (Join-Path $resolvedPackage 'UcalManagerPackage.json') -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $expectedPaths = @($manifest.files | ForEach-Object { [string]$_.path }) + 'UcalManagerPackage.json'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $zipPaths = @($zip.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') })
        if ($zipPaths.Count -ne $expectedPaths.Count -or
            @(Compare-Object $expectedPaths $zipPaths -CaseSensitive).Count) {
            throw 'Portable ZIP contents do not match the package manifest.'
        }
    } finally {
        $zip.Dispose()
    }
    $results.Add([ordered]@{ name = 'portable-zip-matches-manifest'; status = 'passed' })


    $missingLicense = Copy-UcalContractFixture 'missing-license'
    Remove-Item -LiteralPath (Join-Path $missingLicense 'LICENSE') -Force
    $missingLicenseResult = Invoke-UcalContractProcess $manifestGenerator @(
        '-PackageRoot', $missingLicense, '-Version', 'contract-negative')
    Assert-UcalFailure 'missing-license' $missingLicenseResult 'Required distribution license/source file is missing'

    $badNotices = Copy-UcalContractFixture 'tampered-runtime-notice'
    Add-Content -LiteralPath (Join-Path $badNotices 'licenses/ThirdPartyNotices.txt') -Value 'tamper'
    $badNoticesResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $badNotices, '-ExtractionRoot', (Join-Path $testRoot 'bad-notices-extract'), '-SkipFunctionalSmoke')
    Assert-UcalFailure 'tampered-runtime-notice' $badNoticesResult 'Manifest integrity mismatch'

    $rawMetadata = Copy-UcalContractFixture 'raw-license-metadata'
    Set-Content -LiteralPath (Join-Path $rawMetadata 'licenses/qt_attribution.json') -Value '{}'
    $rawMetadataResult = Invoke-UcalContractProcess $manifestGenerator @(
        '-PackageRoot', $rawMetadata, '-Version', 'contract-negative')
    Assert-UcalFailure 'raw-license-metadata' $rawMetadataResult 'Unrecognized data payload in the user release'

    $missingRuntime = Copy-UcalContractFixture 'missing-runtime'
    Remove-Item -LiteralPath (Join-Path $missingRuntime 'runtime\x64\UcalBroker.exe') -Force
    $missingResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $missingRuntime,
        '-ExtractionRoot', (Join-Path $testRoot 'missing-runtime-extract'),
        '-SkipFunctionalSmoke')
    Assert-UcalFailure 'missing-required-runtime' $missingResult 'Manifest file is missing'

    $missingWorker = Copy-UcalContractFixture 'missing-x86-worker'
    Remove-Item -LiteralPath (Join-Path $missingWorker 'runtime\x86\UcalBroker.exe') -Force
    $missingWorkerResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $missingWorker,
        '-ExtractionRoot', (Join-Path $testRoot 'missing-x86-worker-extract'),
        '-SkipFunctionalSmoke')
    Assert-UcalFailure 'missing-required-x86-worker' $missingWorkerResult 'Manifest file is missing'

    $missingDiscovery = Copy-UcalContractFixture 'missing-j2534-discovery-host'
    Remove-Item -LiteralPath (Join-Path $missingDiscovery 'runtime\x64\UcalJ2534DiscoveryHost.exe') -Force
    $missingDiscoveryResult = Invoke-UcalContractProcess $manifestGenerator @(
        '-PackageRoot', $missingDiscovery, '-Version', 'contract-negative')
    Assert-UcalFailure 'missing-j2534-discovery-host' $missingDiscoveryResult 'Required release runtime asset is missing'

    $missingGuiRuntime = Copy-UcalContractFixture 'missing-gui-runtime'
    Remove-Item -LiteralPath (Join-Path $missingGuiRuntime 'libwinpthread-1.dll') -Force
    $missingGuiResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $missingGuiRuntime,
        '-ExtractionRoot', (Join-Path $testRoot 'missing-gui-runtime-extract'),
        '-SkipFunctionalSmoke')
    Assert-UcalFailure 'missing-required-gui-runtime' $missingGuiResult `
        'Manifest file is missing'

    $tampered = Copy-UcalContractFixture 'tampered-payload'
    Add-Content -LiteralPath (Join-Path $tampered 'LICENSE_SCOPE.md') `
        -Value "`n# package-contract tamper"
    $tamperedResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $tampered,
        '-ExtractionRoot', (Join-Path $testRoot 'tampered-extract'),
        '-SkipFunctionalSmoke')
    Assert-UcalFailure 'tampered-payload' $tamperedResult 'Manifest integrity mismatch'

    $unmanifested = Copy-UcalContractFixture 'unmanifested-payload'
    Copy-Item -LiteralPath (Join-Path $unmanifested 'UcalManager.exe') `
        -Destination (Join-Path $unmanifested 'unexpected.exe')
    $unmanifestedResult = Invoke-UcalContractProcess $validator @(
        '-PackageRoot', $unmanifested,
        '-ExtractionRoot', (Join-Path $testRoot 'unmanifested-extract'),
        '-SkipFunctionalSmoke')
    Assert-UcalFailure 'unmanifested-payload' $unmanifestedResult 'Unmanifested package files'

    $vendorRuntime = Copy-UcalContractFixture 'vendor-runtime-name'
    Copy-Item -LiteralPath (Join-Path $vendorRuntime 'UcalManager.exe') `
        -Destination (Join-Path $vendorRuntime 'PCANBasic.dll')
    $vendorResult = Invoke-UcalContractProcess $manifestGenerator @(
        '-PackageRoot', $vendorRuntime, '-Version', 'contract-negative')
    Assert-UcalFailure 'vendor-runtime-name' $vendorResult 'Unrecognized executable payload'

    $wrongCompilerRuntime = Copy-UcalContractFixture 'wrong-compiler-runtime'
    Copy-Item -LiteralPath (Join-Path $wrongCompilerRuntime 'UcalManager.exe') `
        -Destination (Join-Path $wrongCompilerRuntime 'libgcc_s_dw2-1.dll')
    $wrongCompilerResult = Invoke-UcalContractProcess $manifestGenerator @(
        '-PackageRoot', $wrongCompilerRuntime, '-Version', 'contract-negative')
    Assert-UcalFailure 'wrong-compiler-runtime' $wrongCompilerResult `
        'Unrecognized executable payload'

    foreach($negative in @(
        @{Name='missing-vxl-support';Path='frontends/vxlapi/x64/UcalVectorSupport64.dll';Error='Required release runtime asset is missing'},
        @{Name='missing-pcanview-support';Path='frontends/pcanview/x64/UcalPcanViewSupport.dll';Error='Required release runtime asset is missing'},
        @{Name='missing-source-information';Path='SOURCE.txt';Error='Required distribution license/source file is missing'},
        @{Name='missing-worker-crt';Path='runtime/x86/vcruntime140.dll';Error='Missing release DLL dependency'})){
        $fixture=Copy-UcalContractFixture $negative.Name $negative.Path
        $invocation=Invoke-UcalContractProcess $manifestGenerator @('-PackageRoot',$fixture,'-Version','contract-negative')
        Assert-UcalFailure $negative.Name $invocation $negative.Error
    }
    $wrongArch=Copy-UcalContractFixture 'wrong-vxl-support-architecture'
    Copy-Item -LiteralPath (Join-Path $wrongArch 'frontends/vxlapi/x86/UcalVectorSupport.dll') -Destination (Join-Path $wrongArch 'frontends/vxlapi/x64/UcalVectorSupport64.dll') -Force
    Assert-UcalFailure 'wrong-vxl-support-architecture' (Invoke-UcalContractProcess $manifestGenerator @('-PackageRoot',$wrongArch,'-Version','contract-negative')) 'wrong architecture'
    foreach($foreign in @(
        @{Name='foreign-vendor-application';Path='CANoe64.exe';Error='Unrecognized executable payload'},
        @{Name='system-sdk-header';Path='Windows.h';Error='Developer or SDK payload is not allowed'},
        @{Name='engineering-script';Path='Engineering.ps1';Error='Developer or SDK payload is not allowed'})){
        $fixture=Copy-UcalContractFixture $foreign.Name
        Copy-Item -LiteralPath (Join-Path $fixture 'UcalManager.exe') -Destination (Join-Path $fixture $foreign.Path)
        Assert-UcalFailure $foreign.Name (Invoke-UcalContractProcess $manifestGenerator @('-PackageRoot',$fixture,'-Version','contract-negative')) $foreign.Error
    }

    [ordered]@{
        schema = 'org.ucal.manager-package-contract-test/1'
        status = 'passed'
        cases = @($results)
    } | ConvertTo-Json -Depth 6
}
finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot).TrimEnd('\')
    if ($resolvedTestRoot.StartsWith($outRoot + '\', [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTestRoot -PathType Container)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
exit 0
