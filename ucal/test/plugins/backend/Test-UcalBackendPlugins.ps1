[CmdletBinding()]
param([string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
if (!$EvidenceDirectory) { $EvidenceDirectory = Join-Path $root 'out/backend-plugin-deterministic' }
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$EvidenceDirectory = (Resolve-Path $EvidenceDirectory).Path
$names = @('UCAL_BACKEND', 'UCAL_BACKEND_CATALOG', 'UCAL_BROKER_INSTANCE', 'UCAL_J2534_TEST_CASE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$results = @()
try {
    Import-Module (Join-Path $PSScriptRoot '../../../../tools/UcalToolchain.psm1') -Force
    $builder=Get-UcalMSBuildPath
    $legacyLibraries=@{}
    foreach($platform in @('x64','Win32')) {
        $legacyDirectory=Join-Path $EvidenceDirectory "legacy-runtime-$platform"
        New-Item -ItemType Directory -Force $legacyDirectory | Out-Null
        $null=Invoke-UcalLoggedProcess $builder @((Join-Path $root 'ucal/test/plugins/backend/mocks/UcalBackendMock.vcxproj'),
            '/nologo','/verbosity:minimal','/p:Configuration=Release',"/p:Platform=$platform",
            "/p:SolutionDir=$($root.Replace('\','/'))/",'/p:UcalFixtureLegacyAbi=true',
            "/p:OutDir=$($legacyDirectory.Replace('\','/'))/",
            "/p:IntDir=$($legacyDirectory.Replace('\','/'))/obj/") $root (Join-Path $legacyDirectory 'build.log') -TimeoutSeconds 300
        $legacyLibraries[$platform]=Join-Path $legacyDirectory 'UcalBackendLegacyFixture.dll'
    }
    foreach ($pair in @(@('x64','x64'), @('Win32','Win32'), @('x64','Win32'))) {
        $hostArch, $workerArch = $pair
        $hostBin = Join-Path $root "out/$hostArch/Release"
        $workerBin = Join-Path $root "out/$workerArch/Release"
        foreach ($case in @('multiple','refresh','deferred','empty','invalid','crash-read','hang-read','j2534','legacy-entrypoint')) {
            $folder = Join-Path $EvidenceDirectory "$hostArch-$workerArch-$case"
            New-Item -ItemType Directory -Force -Path $folder | Out-Null
            $catalog = Join-Path $folder 'catalog.ini'
            $count = if ($case -eq 'empty') { 0 } elseif ($case -eq 'invalid') { 1 } else { 2 }
            $text = "[catalog]`nversion=1`ncount=$count`n"
            $env:UCAL_J2534_TEST_CASE = if ($case -eq 'j2534') { 'standard-null' } else { '' }
            for ($i=0; $i -lt $count; ++$i) {
                $id = if ($case -eq 'invalid') { 'bad' } elseif ($i -eq 0) { 'a' } else { 'b' }
                $config = Join-Path $folder "$id.ini"
                $mode = 'ucal'
                $library = Join-Path $workerBin 'UcalBackendFixture.dll'
                if ($case -eq 'legacy-entrypoint') { $library=$legacyLibraries[$workerArch] }
                $options = "[fixture]`nchannels=4`n"
                if ($case -in @('crash-read','hang-read')) {
                    $options = "[fixture]`nchannels=1`nfault=$(if ($i -eq 0) { $case } else { '' })`n"
                }
                if ($case -eq 'invalid') { $library = Join-Path $workerBin 'UcalJ2534TestRuntime.dll' }
                if ($case -eq 'j2534') {
                    $mode = 'j2534'
                    $library = Join-Path $workerBin 'UcalBackendJ2534.dll'
                    $runtime = Join-Path $workerBin 'UcalJ2534StandardRuntime.dll'
                    $options = "[provider]`nprofile=04.04-gmw17753-raw-iso-fd`nid=$id`nruntime=$runtime`nendpoint_count=1`npins=1550`n[endpoint.0]`nopen_mode=default`nselector=`nadapter_identity=port`nphysical_port=0`nname=Standard-only fixture`nmodel=Original virtual test`nserial=fixture`nbrs=1`ntermination=1`n"
                }
                $options | Set-Content -LiteralPath $config -Encoding Unicode
                $text += "[plugin.$i]`nid=$id`nfamily=$mode`nmode=$mode`nlibrary=$library`nconfiguration=$config`nworker=$(Join-Path $workerBin 'UcalBroker.exe')`n"
            }
            $text | Set-Content -LiteralPath $catalog -Encoding Unicode
            $env:UCAL_BACKEND = 'PLUGINS'
            $env:UCAL_BACKEND_CATALOG = $catalog
            $env:UCAL_BROKER_INSTANCE = 'plugin-test-' + [Guid]::NewGuid().ToString('N')
            $process = Start-Process -FilePath (Join-Path $hostBin 'UcalBroker.exe') -ArgumentList '--allow-test-shutdown' -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'broker.log') -RedirectStandardError (Join-Path $folder 'broker.stderr.log')
            try {
                $ready = $false
                for ($attempt=0; $attempt -lt 60; ++$attempt) {
                    & (Join-Path $hostBin 'UcalHarness.exe') ping *> $null
                    if ($LASTEXITCODE -eq 0) { $ready = $true; break }
                    if ($process.HasExited) { break }
                    Start-Sleep -Milliseconds 50
                }
                if (!$ready) { throw "Broker startup failed: $folder" }
                $scenario = if ($case -in @('crash-read','hang-read')) { 'fault' } elseif ($case -eq 'legacy-entrypoint') { 'multiple' } else { $case }
                & (Join-Path $hostBin 'UcalPluginHarness.exe') $scenario *> (Join-Path $folder 'result.txt')
                $exitCode = $LASTEXITCODE
                $results += [ordered]@{host=$hostArch;worker=$workerArch;case=$case;exitCode=$exitCode}
                if ($exitCode -ne 0) { throw "Backend plugin test failed: $folder" }
            } finally {
                & (Join-Path $hostBin 'UcalHarness.exe') shutdown *> $null
                if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id }
                $process.Dispose()
            }
        }
    }
    [ordered]@{status='passed';cases=$results} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $EvidenceDirectory 'result.json') -Encoding UTF8
    Write-Output "Backend plugins: $($results.Count) cases passed"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
