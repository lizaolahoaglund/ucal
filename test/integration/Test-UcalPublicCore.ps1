[CmdletBinding()]
param([ValidateRange(1,100)][int]$RestartIterations = 2,
      [string]$MSBuildPath = $env:UCAL_MSBUILD)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $root 'tools/UcalToolchain.psm1') -Force
$builder = Get-UcalMSBuildPath $MSBuildPath
$saved = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'UCAL_*' } | ForEach-Object {
    $saved[$_.Name] = $_.Value
    [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
}
$output = Join-Path $root ('out/test-logs/public-core-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$cases = @()
function Assert-Exit([string]$Id) {
    if ($LASTEXITCODE -ne 0) { throw "$Id failed with exit code $LASTEXITCODE" }
    $script:cases += [ordered]@{id=$Id;status='passed'}
}
function Start-FixtureBroker([string]$Id, [string]$Executable) {
    $process = Start-Process -FilePath $Executable -ArgumentList '--allow-test-shutdown' `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $output "$Id.stdout.log") `
        -RedirectStandardError (Join-Path $output "$Id.stderr.log")
    $null = $process.Handle
    return $process
}
try {
    foreach ($configuration in @('Debug','Release')) {
        foreach ($platform in @('x64','Win32')) {
            & $builder (Join-Path $root 'Ucal.sln') /m /nologo /verbosity:minimal `
                "/p:Configuration=$configuration" "/p:Platform=$platform"
            Assert-Exit "build-core-$platform-$configuration"
            & (Join-Path $root 'tools/Build-UcalPlugins.ps1') -Configuration $configuration `
                -Platform $platform -MSBuildPath $builder -IncludeTests -PublicSourceOnly
            Assert-Exit "build-public-modules-$platform-$configuration"
        }
    }
    foreach ($platform in @('x64','Win32')) {
        $bin = Join-Path $root "out/$platform/Release"
        $harness = Join-Path $bin 'UcalHarness.exe'
        $pcanHarness = Join-Path $bin 'UcalPcanHarness.exe'
        $frontend = Join-Path $bin 'PCANBasic.dll'
        $broker = Join-Path $bin 'UcalBroker.exe'
        foreach ($command in @('protocol-decoders','advanced-timing-contract','clock-domains','vector-frontend-catalog')) {
            & $harness $command
            Assert-Exit "$platform-$command"
        }
        $env:UCAL_PCAN_BASIC_RUNTIME = Join-Path $bin 'UcalPcanBasicTestRuntime.dll'
        $env:UCAL_PCAN_ALLOW_ECHO_FRAMES = '1'
        $env:UCAL_VECTOR_XL_RUNTIME = Join-Path $bin 'UcalVectorXlTestRuntime.dll'
        foreach ($backend in @('PCAN','VECTOR')) {
            $env:UCAL_BACKEND = $backend
            $env:UCAL_BROKER_INSTANCE = 'public_core_' + [Guid]::NewGuid().ToString('N')
            $id = "$platform-$backend-data-plane"
            $process = Start-FixtureBroker $id $broker
            try {
                & $harness $(if ($backend -eq 'PCAN') { 'pcan-mock' } else { 'vector-mock' })
                Assert-Exit $id
                & $pcanHarness $frontend data-plane
                Assert-Exit "$platform-pcan-frontend-$backend"
                & $harness shutdown
                Assert-Exit "$id-shutdown"
                if (-not $process.WaitForExit(5000)) { throw "$id did not exit" }
            } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id } }
            foreach ($timingMode in @('precise','nominal-only')) {
                $env:UCAL_TEST_EXPECT_TIMING = $timingMode
                $id = "$platform-$backend-$timingMode"
                $process = Start-FixtureBroker $id $broker
                try {
                    & $pcanHarness $frontend $(if ($timingMode -eq 'precise') { 'advanced-timing' } else { 'nominal-only' })
                    Assert-Exit $id
                    & $harness shutdown
                    Assert-Exit "$id-shutdown"
                    if (-not $process.WaitForExit(5000)) { throw "$id did not exit" }
                } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id } }
            }
            $env:UCAL_TEST_EXPECT_TIMING = $null
        }
        $env:UCAL_BACKEND = 'PCAN'
        $env:UCAL_PCAN_TEST_VISIBILITY_FILE = Join-Path $output "$platform-topology.txt"
        [IO.File]::WriteAllText($env:UCAL_PCAN_TEST_VISIBILITY_FILE, 'both')
        $process = Start-FixtureBroker "$platform-topology" $broker
        try {
            & $pcanHarness $frontend topology
            Assert-Exit "$platform-stable-pcan-handles"
            & $harness shutdown
            Assert-Exit "$platform-topology-shutdown"
            if (-not $process.WaitForExit(5000)) { throw 'Topology Broker did not exit' }
        } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id } }
        $env:UCAL_PCAN_TEST_VISIBILITY_FILE = $null
    }
    $env:UCAL_BACKEND = $null
    $env:UCAL_PCAN_BASIC_RUNTIME = $null
    $env:UCAL_VECTOR_XL_RUNTIME = $null
    $env:UCAL_BROKER_INSTANCE = 'public_core_' + [Guid]::NewGuid().ToString('N')
    foreach ($platform in @('x64','Win32')) {
        $bin = Join-Path $root "out/$platform/Release"
        $harness = Join-Path $bin 'UcalHarness.exe'
        $baseline = $null
        for ($iteration = 1; $iteration -le $RestartIterations; ++$iteration) {
            $process = Start-FixtureBroker "$platform-restart-$iteration" (Join-Path $bin 'UcalBroker.exe')
            try {
                $inventory = (& $harness enumerate | Out-String).Trim()
                Assert-Exit "$platform-restart-$iteration-enumeration"
                if ($null -eq $baseline) { $baseline = $inventory }
                if ($inventory -ne $baseline) { throw 'Inventory identity changed after Broker restart' }
                & $harness lifecycle
                Assert-Exit "$platform-restart-$iteration-lifecycle"
                & $harness shutdown
                Assert-Exit "$platform-restart-$iteration-shutdown"
                if (-not $process.WaitForExit(5000)) { throw 'Restart Broker did not exit' }
            } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id } }
        }
    }
    & (Join-Path $root 'ucal/test/host/Test-UcalBrokerConcurrency.ps1')
    & (Join-Path $root 'ucal/test/j2534/Test-UcalJ2534Backend.ps1')
    & (Join-Path $root 'ucal/test/j2534/Test-UcalJ2534Discovery.ps1')
    & (Join-Path $root 'ucal/test/plugins/backend/Test-UcalBackendPlugins.ps1')
    & (Join-Path $root 'ucal/test/Test-UcalTools.ps1')
    & (Join-Path $root 'test/Test-UcalApiClientDeployment.ps1') -Api PcanBasic
    & (Join-Path $root 'frontends/pcanview/test/Test-UcalPcanViewSupport.ps1') -Root $root
    $powerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $smoke = Join-Path $root 'ucal/test/hardware/Invoke-UcalCanHardwareSmoke.ps1'
    foreach ($backend in @('PCAN','VECTOR')) {
        $plan = & $powerShell -NoProfile -ExecutionPolicy Bypass -File $smoke -Backend $backend -Mode Plan
        Assert-Exit "hardware-plan-$backend"
        if (($plan | ConvertFrom-Json).status -ne 'planned') { throw "Missing hardware plan: $backend" }
    }
    $summary = [ordered]@{schema='org.ucal.public-core-regression/1';status='passed';
        hardwareAccess=$false;cases=$cases;
        privateCompatibilityQualification='not-requested: separate support implementation or binaries required'}
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'), ($summary | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    $summary | ConvertTo-Json -Depth 6
} finally {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'UCAL_*' } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
# Do not propagate an expected negative fixture's native status as this gate's exit.
$global:LASTEXITCODE = 0
