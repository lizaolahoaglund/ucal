[CmdletBinding()]
param(
    [ValidateSet('x64','Win32')][string[]]$Architecture = @('x64','Win32'),
    [string]$EvidenceDirectory,
    [switch]$SkipCrossBitness
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $EvidenceDirectory) { $EvidenceDirectory = Join-Path $root 'out\j2534-deterministic' }
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$EvidenceDirectory = (Resolve-Path $EvidenceDirectory).Path
$names = @('UCAL_J2534_CONFIG','UCAL_J2534_WORKER_PATH','UCAL_J2534_TEST_CASE','UCAL_IPC_DIAGNOSTICS')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$results = @()
try {
    $env:UCAL_IPC_DIAGNOSTICS = '1'
    foreach ($arch in $Architecture) {
        $bin = Join-Path $root "out\$arch\Release"
        $runtime = Join-Path $bin 'UcalJ2534TestRuntime.dll'
        $worker = Join-Path $bin 'UcalBroker.exe'
        $harness = Join-Path $bin 'UcalJ2534Harness.exe'
        foreach ($file in @($runtime,$worker,$harness)) {
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing built artifact: $file" }
        }
        $config = Join-Path $EvidenceDirectory "$arch.ini"
        @"
[provider]
profile=04.04-gmw17753-raw-iso-fd
id=ucal_test_fixture
runtime=$runtime
endpoint_count=1
pins=1550
[endpoint.0]
selector=fixture-device:0
adapter_identity=fixture-device
physical_port=0
name=Original J2534 deterministic fixture
model=UCAL fixture
serial=fixture
brs=1
termination=1
"@ | Set-Content -LiteralPath $config -Encoding Unicode
        $env:UCAL_J2534_CONFIG = $config
        $env:UCAL_J2534_WORKER_PATH = $worker
        $baseConfig = Get-Content -LiteralPath $config -Raw
        $cases = @('normal','native-config-list','zero-handles','wrong-api','sae-only','classic-only','isotp-only',
            'wrong-readback','no-data-rate','filter-failure','busy','partial',
            'oversized-count','malformed-rx','sae-flags','echo','error-indication',
            'esi','wrap','overflow-rx','overflow-full','backward-clock',
            'fixed-match','fixed-mismatch','fixed-version','fixed-rate-mismatch','hang-open','hang-read','crash-read',
            'dynamic','timing-match','timing-mismatch','ports','ports-malformed','hot-refresh',
            'disconnected-read','disconnected-write','disconnected-close','timeout-write','failed-write',
            'multi-named','multi-named-classic','multi-shared','multi-shared-classic','multi-zero',
            'multi-rollback','multi-filter-rollback','multi-route-change','multi-malformed','multi-duplicate-route')
        foreach ($case in $cases) {
            $caseConfig = $baseConfig
            if ($case -eq 'native-config-list') {
                $caseConfig = $caseConfig.Replace('pins=1550', ('pins=1550' + [Environment]::NewLine + 'config_list_layout=native'))
            }
            if ($case -eq 'dynamic' -or $case -like 'multi-*') { $caseConfig = $caseConfig.Replace('endpoint_count=1', "endpoint_count=0`ndiscovery_library=$runtime") }
            if ($case -like 'timing-*' -or $case -like 'ports*') {
                $caseConfig = $caseConfig.Replace('pins=1550', "pins=1550`ntiming_query=UcalFixtureGetCanTiming")
            }
            if ($case -like 'ports*') {
                $caseConfig = $caseConfig.Replace('pins=1550', "pins=1550`ndiscovery_query=UcalFixtureEnumeratePorts").Replace(
                    'fixture-device:0', '000000000000000000000001:0')
            }
            if ($case -like 'fixed-*') {
                $version = if ($case -eq 'fixed-version') { '9.9.9.9' } else { '0.0.0.0' }
                $hash = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
                $caseConfig = $baseConfig.Replace('pins=1550',
                    "pins=1550`nfixed_fd_nominal_sample_point=80`nfixed_fd_data_sample_point=75`nfixed_fd_nominal_bitrate=500000`nfixed_fd_data_bitrate=2000000`nfixed_timing_dll_version=$version`nfixed_timing_sha256=$hash")
            }
            Set-Content -LiteralPath $config -Value $caseConfig -Encoding Unicode
            $stdout = Join-Path $EvidenceDirectory "$arch-$case.stdout.jsonl"
            $stderr = Join-Path $EvidenceDirectory "$arch-$case.stderr.log"
            $p = Start-Process -FilePath $harness -ArgumentList $case -PassThru -WindowStyle Hidden `
                -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            # Keep the native process handle alive so Windows PowerShell 5.1 can
            # retrieve the exit code even when a short case has already exited.
            $null = $p.Handle
            if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id; throw "Timed out: $arch/$case" }
            $p.WaitForExit()
            $results += [pscustomobject]@{ architecture=$arch; case=$case; exitCode=$p.ExitCode; output=$stdout }
            if ($p.ExitCode -ne 0) { throw "J2534 test failed: $arch/$case; see $stderr" }
            if ($case -in @('hang-read','malformed-rx')) {
                $failure = @(Get-Content -LiteralPath $stderr | ForEach-Object {
                    $_ | ConvertFrom-Json
                } | Where-Object { $_.event -eq 'worker_request_failure' })
                $expectedStage = if ($case -eq 'hang-read') { 'read-header' } else { 'backend-response' }
                if ($failure.Count -ne 1 -or $failure[0].stage -ne $expectedStage -or
                    $failure[0].operation -ne 11 -or $failure[0].elapsedMs -lt 0) {
                    throw "Worker failure origin was not distinguished: $arch/$case"
                }
                $expectedError = if ($case -eq 'hang-read') { 1460 } else { 0 }
                if ($failure[0].win32Error -ne $expectedError) {
                    throw "Worker diagnostic Win32 error was not truthful: $arch/$case"
                }
            }
        }
        Import-Module (Join-Path $root 'tools/UcalToolchain.psm1')
        $msbuild = Get-UcalMSBuildPath
        & $msbuild (Join-Path $root 'ucal\test\j2534\mocks\transport\UcalJ2534TestRuntime.vcxproj') /m:1 /nologo /verbosity:minimal `
            /p:Configuration=Release "/p:Platform=$arch" /p:UcalJ2534StandardOnly=true "/p:SolutionDir=$($root.Replace('\', '/'))/"
        if ($LASTEXITCODE -ne 0) { throw "Standard-only runtime build failed: $arch" }
        $opaqueRuntime = Join-Path $EvidenceDirectory "$arch-unknown-provider.dll"
        Copy-Item -LiteralPath (Join-Path $bin 'UcalJ2534StandardRuntime.dll') -Destination $opaqueRuntime -Force
        $genericConfig = $baseConfig.Replace($runtime, $opaqueRuntime).Replace('id=ucal_test_fixture', 'id=unknown-provider')
        $genericConfig = $genericConfig.Replace('selector=fixture-device:0', "open_mode=default`nselector=")
        Set-Content -LiteralPath $config -Value $genericConfig -Encoding Unicode
        foreach ($case in @('standard-null','standard-set-only','standard-get-busy','standard-default-pins','standard-read-only',
                              'standard-unsupported-termination','standard-invalid-value')) {
            $stdout = Join-Path $EvidenceDirectory "$arch-$case.stdout.jsonl"
            $stderr = Join-Path $EvidenceDirectory "$arch-$case.stderr.log"
            $p = Start-Process -FilePath $harness -ArgumentList $case -PassThru -WindowStyle Hidden `
                -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            $null = $p.Handle
            if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id; throw "Timed out: $arch/$case" }
            $p.WaitForExit()
            $results += [pscustomobject]@{architecture=$arch;case=$case;exitCode=$p.ExitCode;output=$stdout}
            if ($p.ExitCode -ne 0) { throw "Standard-only case failed: $arch/$case; see $stderr" }
        }
        & $msbuild (Join-Path $root 'ucal\test\j2534\mocks\transport\UcalJ2534TestRuntime.vcxproj') /m:1 /nologo /verbosity:minimal `
            /p:Configuration=Release "/p:Platform=$arch" /p:UcalJ2534DiscoveryV1Only=true "/p:SolutionDir=$($root.Replace('\', '/'))/"
        if ($LASTEXITCODE -ne 0) { throw "V1 discovery runtime build failed: $arch" }
        $v1Runtime = Join-Path $bin 'UcalJ2534V1Runtime.dll'
        $v1Config = $baseConfig.Replace($runtime, $v1Runtime).Replace('endpoint_count=1', "endpoint_count=0`ndiscovery_library=$v1Runtime")
        Set-Content -LiteralPath $config -Value $v1Config -Encoding Unicode
        & $harness dynamic
        if ($LASTEXITCODE -ne 0) { throw "V1 discovery compatibility failed: $arch" }
        $results += [pscustomobject]@{architecture=$arch;case='discovery-v1-compatibility';exitCode=0}
        if (-not $SkipCrossBitness) {
            Set-Content -LiteralPath $config -Value $baseConfig -Encoding Unicode
            $other = if ($arch -eq 'x64') { 'Win32' } else { 'x64' }
            $otherHarness = Join-Path $root "out\$other\Release\UcalJ2534Harness.exe"
            & $otherHarness normal
            if ($LASTEXITCODE -ne 0) { throw "Cross-bitness test failed: $other client -> $arch worker" }
            $results += [pscustomobject]@{architecture="$other-to-$arch";case='cross-bitness';exitCode=0}
            Set-Content -LiteralPath $config -Value $genericConfig -Encoding Unicode
            & $otherHarness standard-set-only
            if ($LASTEXITCODE -ne 0) { throw "Standard-only cross-bitness test failed: $other -> $arch" }
            $results += [pscustomobject]@{architecture="$other-to-$arch";case='standard-cross-bitness';exitCode=0}
            $multiConfig = $baseConfig.Replace('endpoint_count=1', "endpoint_count=0`ndiscovery_library=$runtime")
            Set-Content -LiteralPath $config -Value $multiConfig -Encoding Unicode
            foreach($multiCase in @('multi-named','multi-shared','multi-shared-classic')) {
                & $otherHarness $multiCase
                if($LASTEXITCODE -ne 0){throw "Multichannel cross-bitness failed: $other -> $arch / $multiCase"}
                $results += [pscustomobject]@{architecture="$other-to-$arch";case=$multiCase;exitCode=0}
            }
        }
    }
    [ordered]@{status='passed';hardware=$false;cases=$results} | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'result.json') -Encoding UTF8
    Write-Output "J2534 deterministic cases passed: $($results.Count)"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
