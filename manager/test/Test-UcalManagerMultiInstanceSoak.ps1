[CmdletBinding()]
param(
    [string]$SwitchPath,
    [string]$OutputDirectory,
    [ValidateRange(2, 8)][int]$InstanceCount = 4,
    [ValidateRange(60, 3600)][int]$DurationSeconds = 600,
    [ValidateRange(10, 120)][int]$SampleIntervalSeconds = 60,
    [ValidateRange(1048576, 268435456)][int64]$MaximumPrivateGrowthBytes = 33554432,
    [ValidateRange(1, 256)][int]$MaximumHandleGrowth = 32
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($SwitchPath)) {
    $SwitchPath = Join-Path $repoRoot 'out\UcalManager-product-package\UcalManager.exe'
}
if (-not (Test-Path -LiteralPath $SwitchPath -PathType Leaf)) {
    throw "UCAL Manager package executable was not found: $SwitchPath"
}
$SwitchPath = (Resolve-Path -LiteralPath $SwitchPath).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot (
        'out\switch-multi-instance-soak\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$testBrokerInstance = 'ucal_soak_test_' + [guid]::NewGuid().ToString('N')
$testSettings = Join-Path $OutputDirectory 'settings'
$emptyBackends = Join-Path $OutputDirectory 'empty-backends'
New-Item -ItemType Directory -Force -Path $testSettings,$emptyBackends | Out-Null
$testBrokerPaths = @('runtime/x64/UcalBroker.exe','runtime/x86/UcalBroker.exe') | ForEach-Object {
    [IO.Path]::GetFullPath((Join-Path (Split-Path $SwitchPath -Parent) $_))
}
$ownedManagerIds = [Collections.Generic.List[int]]::new()

function Quote-Argument([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\**)"', '$1$1\"' -replace '(\+)$', '$1$1') + '"'
}

function Start-OwnedSwitch([int]$Slot, [int]$Generation) {
    $pages = @('applications', 'hardware', 'software', 'drivers', 'settings')
    $workspace = Join-Path $OutputDirectory ("instance-{0}-workspace.json" -f $Slot)
    $arguments = @('--presentation', 'production', '--language', 'en',
        '--workspace', $workspace, '--page', $pages[$Slot % $pages.Count])
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $SwitchPath
    $info.WorkingDirectory = Split-Path -Parent $SwitchPath
    $info.UseShellExecute = $false
    $info.EnvironmentVariables['UCAL_BROKER_INSTANCE'] = $testBrokerInstance
    $info.EnvironmentVariables['UCAL_MANAGER_SETTINGS_DIRECTORY'] = $testSettings
    $info.EnvironmentVariables['UCAL_BACKEND_DIRECTORY'] = $emptyBackends
    $info.Arguments = (($arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Could not start UCAL Manager instance $Slot." }
    $ownedManagerIds.Add($process.Id)
    return [pscustomobject]@{
        Slot = $Slot
        Generation = $Generation
        Workspace = $workspace
        Process = $process
    }
}

$instances = [Collections.ArrayList]::new()
$samples = [Collections.Generic.List[object]]::new()
$startedUtc = [DateTimeOffset]::UtcNow
$restartPerformed = $false
$result = [ordered]@{
    schema = 'org.ucal.manager-multi-instance-soak/1'
    status = 'error'
    executable = [ordered]@{
        path = $SwitchPath
        sha256 = (Get-FileHash -LiteralPath $SwitchPath -Algorithm SHA256).Hash
    }
    instanceCount = $InstanceCount
    durationSeconds = $DurationSeconds
    sampleIntervalSeconds = $SampleIntervalSeconds
    maximumPrivateGrowthBytes = $MaximumPrivateGrowthBytes
    maximumHandleGrowth = $MaximumHandleGrowth
    crashRestartSlot = 0
    crashIsolated = $false
    restartPassed = $false
    sampleCount = 0
    resources = @()
    error = $null
}

try {
    for ($slot = 0; $slot -lt $InstanceCount; ++$slot) {
        [void]$instances.Add((Start-OwnedSwitch -Slot $slot -Generation 1))
    }
    Start-Sleep -Seconds 3
    $clock = [Diagnostics.Stopwatch]::StartNew()

    while ($true) {
        foreach ($instance in @($instances)) {
            $instance.Process.Refresh()
            if ($instance.Process.HasExited) {
                throw "Switch slot $($instance.Slot) generation $($instance.Generation) exited unexpectedly."
            }
            if (-not $instance.Process.Responding) {
                throw "Switch slot $($instance.Slot) generation $($instance.Generation) stopped responding."
            }
            $samples.Add([pscustomobject]@{
                elapsedSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
                sampledUtc = [DateTimeOffset]::UtcNow.ToString('o')
                slot = $instance.Slot
                generation = $instance.Generation
                pid = $instance.Process.Id
                handles = $instance.Process.HandleCount
                privateBytes = $instance.Process.PrivateMemorySize64
                workingSetBytes = $instance.Process.WorkingSet64
                responding = $instance.Process.Responding
            })
        }

        if (-not $restartPerformed -and
            $clock.Elapsed.TotalSeconds -ge ($DurationSeconds / 2.0)) {
            $victim = @($instances | Where-Object { $_.Slot -eq 0 })[0]
            $victim.Process.Kill()
            $victim.Process.WaitForExit()
            $victim.Process.Dispose()
            [void]$instances.Remove($victim)
            foreach ($survivor in @($instances)) {
                $survivor.Process.Refresh()
                if ($survivor.Process.HasExited -or -not $survivor.Process.Responding) {
                    throw 'Terminating one exact owned instance affected a survivor.'
                }
            }
            $result.crashIsolated = $true
            [void]$instances.Add((Start-OwnedSwitch -Slot 0 -Generation 2))
            Start-Sleep -Seconds 3
            $restarted = @($instances | Where-Object { $_.Slot -eq 0 })[0]
            $restarted.Process.Refresh()
            if ($restarted.Process.HasExited -or -not $restarted.Process.Responding) {
                throw 'The terminated workspace did not restart into a responsive process.'
            }
            $result.restartPassed = $true
            $restartPerformed = $true
        }

        $remaining = $DurationSeconds - $clock.Elapsed.TotalSeconds
        if ($remaining -le 0) { break }
        Start-Sleep -Seconds ([Math]::Max(1, [Math]::Min($SampleIntervalSeconds,
                                                         [Math]::Ceiling($remaining))))
    }

    foreach ($instance in @($instances)) {
        $instance.Process.Refresh()
        if ($instance.Process.HasExited -or -not $instance.Process.Responding) {
            throw "Switch slot $($instance.Slot) failed the final function check."
        }
        if (-not (Test-Path -LiteralPath $instance.Workspace -PathType Leaf)) {
            throw "Switch slot $($instance.Slot) did not preserve its workspace."
        }
    }

    $resourceResults = [Collections.Generic.List[object]]::new()
    foreach ($group in @($samples | Group-Object slot, generation)) {
        $ordered = @($group.Group | Sort-Object elapsedSeconds)
        if ($ordered.Count -lt 2) { continue }
        $first = $ordered[0]
        $last = $ordered[-1]
        $privateGrowth = [int64]$last.privateBytes - [int64]$first.privateBytes
        $handleGrowth = [int]$last.handles - [int]$first.handles
        $minutes = [Math]::Max(1.0 / 60.0,
            ([double]$last.elapsedSeconds - [double]$first.elapsedSeconds) / 60.0)
        $positiveSlope = [Math]::Max(0.0, $privateGrowth / $minutes)
        $resource = [ordered]@{
            slot = [int]$first.slot
            generation = [int]$first.generation
            pid = [int]$first.pid
            samples = $ordered.Count
            firstPrivateBytes = [int64]$first.privateBytes
            lastPrivateBytes = [int64]$last.privateBytes
            privateGrowthBytes = $privateGrowth
            positivePrivateGrowthBytesPerMinute = [Math]::Round($positiveSlope, 3)
            firstHandles = [int]$first.handles
            lastHandles = [int]$last.handles
            handleGrowth = $handleGrowth
            passed = ($privateGrowth -le $MaximumPrivateGrowthBytes -and
                      $positiveSlope -le 2097152 -and
                      $handleGrowth -le $MaximumHandleGrowth)
        }
        $resourceResults.Add($resource)
        if (-not $resource.passed) {
            throw "Resource trend exceeded the bounded gate for slot $($resource.slot) generation $($resource.generation)."
        }
    }
    if (-not $restartPerformed -or -not $result.crashIsolated -or -not $result.restartPassed) {
        throw 'The bounded crash/restart checkpoint did not run.'
    }
    $result.sampleCount = $samples.Count
    $result.resources = @($resourceResults)
    $result.status = 'passed'
} catch {
    $result.error = $_.Exception.Message
} finally {
    foreach ($instance in @($instances)) {
        try {
            $instance.Process.Refresh()
            if (-not $instance.Process.HasExited) {
                $instance.Process.Kill()
                $instance.Process.WaitForExit()
            }
            $instance.Process.Dispose()
        } catch {}
    }
    # Include previous generations: their Managers are deliberately terminated
    # at the crash checkpoint, but their exact test-owned Broker may outlive them.
    foreach ($broker in @(Get-CimInstance Win32_Process -Filter "Name='UcalBroker.exe'" |
        Where-Object { $_.ParentProcessId -in $ownedManagerIds -and $_.ExecutablePath -in $testBrokerPaths })) {
        try {
            Stop-Process -Id $broker.ProcessId -ErrorAction Stop
            Wait-Process -Id $broker.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
        } catch {
            $result.status = 'error'
            $result.error = 'Test-owned Broker cleanup failed: ' + $_.Exception.Message
        }
    }
}

$result['startedUtc'] = $startedUtc.ToString('o')
$result['completedUtc'] = [DateTimeOffset]::UtcNow.ToString('o')
$samplesPath = Join-Path $OutputDirectory 'samples.json'
$manifestPath = Join-Path $OutputDirectory 'manifest.json'
[IO.File]::WriteAllText($samplesPath, (@($samples) | ConvertTo-Json -Depth 5),
                        [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($manifestPath, ($result | ConvertTo-Json -Depth 7),
                        [Text.UTF8Encoding]::new($false))
$result['samplesPath'] = $samplesPath
$result['manifestPath'] = $manifestPath
$result | ConvertTo-Json -Depth 7 -Compress
if ($result.status -ne 'passed') { exit 1 }
