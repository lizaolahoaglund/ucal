[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Execute')]
    [string]$Mode = 'Plan',

    [ValidateNotNullOrEmpty()]
    [string]$Backend,

    [Parameter(Mandatory = $true)]
    [string]$EndpointA,

    [Parameter(Mandatory = $true)]
    [string]$EndpointB,

    [Parameter(Mandatory = $true)]
    [string]$RuntimePath,

    [ValidateSet('Unspecified', 'Auto', 'On', 'Off')]
    [string]$Termination = 'Unspecified',

    [switch]$CanFd,

    [ValidateRange(1000, 10000000)]
    [uint32]$NominalBitrate = 500000,

    [ValidateRange(1000, 10000000)]
    [uint32]$DataBitrate = 2000000,

    [ValidateRange(0, 64)]
    [uint32]$PayloadLength = 0,

    [ValidateRange(1, 100000)]
    [uint32]$FrameCount = 1000,

    [ValidateRange(0, 1000000)]
    [uint32]$InterFrameDelayUs = 1000,

    [ValidateRange(1, 10000)]
    [uint32]$EventPollEveryFrames = 1,

    [ValidateRange(100, 60000)]
    [uint32]$TimeoutMs = 30000,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP95LatencyUs = 0,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP99LatencyUs = 0,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP999LatencyUs = 0,

    [ValidateRange(100, 10000)]
    [uint32]$StartDelayMs = 2000,

    [ValidateRange(0, 2147483647)]
    [int64]$MaximumPrivateGrowthBytes = 0,

    [ValidateRange(0, 10000)]
    [int]$MaximumHandleGrowth = 0,

    [ValidateRange(0, 3600)]
    [int]$ResourceSampleSeconds = 0,

    [ValidateRange(0, 2147483647)]
    [int64]$MaximumPrivateTrendBytesPerMinute = 0,

    [string]$BrokerPath,
    [string]$HarnessPath,
    [string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Import-Module (Join-Path $PSScriptRoot 'Hardware.psm1') -Force
$backendPolicy = Get-UcalHardwareTestProfile $repoRoot $Backend
if ([string]::IsNullOrWhiteSpace($BrokerPath)) {
    $BrokerPath = Join-Path $repoRoot 'out\x64\Release\UcalBroker.exe'
}
if ([string]::IsNullOrWhiteSpace($HarnessPath)) {
    $HarnessPath = Join-Path $repoRoot 'out\x64\Release\UcalHarness.exe'
}

function Resolve-RequiredFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-ArtifactIdentity([string]$Path) {
    return [ordered]@{
        path = $Path
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Set-ProcessEnvironment([string]$Name, [AllowNull()][string]$Value) {
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}

function ConvertFrom-JsonLines([string]$Text) {
    $items = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $items += $line | ConvertFrom-Json
        }
    }
    return @($items)
}

function Update-ProcessMetrics([Diagnostics.Process]$Process, [hashtable]$Metrics) {
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        $key = [string]$Process.Id
        if (-not $Metrics.ContainsKey($key)) {
            $Metrics[$key] = [ordered]@{
                pid = $Process.Id
                name = $Process.ProcessName
                samples = 0
                firstWorkingSetBytes = [int64]$Process.WorkingSet64
                firstPrivateBytes = [int64]$Process.PrivateMemorySize64
                firstHandleCount = [int]$Process.HandleCount
                lastWorkingSetBytes = [int64]$Process.WorkingSet64
                lastPrivateBytes = [int64]$Process.PrivateMemorySize64
                lastHandleCount = [int]$Process.HandleCount
                peakWorkingSetBytes = 0L
                peakPrivateBytes = 0L
                peakHandleCount = 0
                peakThreadCount = 0
            }
        }
        $metric = $Metrics[$key]
        $metric.samples++
        $metric.lastWorkingSetBytes = [int64]$Process.WorkingSet64
        $metric.lastPrivateBytes = [int64]$Process.PrivateMemorySize64
        $metric.lastHandleCount = [int]$Process.HandleCount
        $metric.peakWorkingSetBytes = [Math]::Max(
            [int64]$metric.peakWorkingSetBytes, [int64]$Process.WorkingSet64)
        $metric.peakPrivateBytes = [Math]::Max(
            [int64]$metric.peakPrivateBytes, [int64]$Process.PrivateMemorySize64)
        $metric.peakHandleCount = [Math]::Max(
            [int]$metric.peakHandleCount, [int]$Process.HandleCount)
        $metric.peakThreadCount = [Math]::Max(
            [int]$metric.peakThreadCount, [int]$Process.Threads.Count)
    } catch {
        # A process may exit between Refresh and a counter read. Its previous peak remains valid.
    }
}

function Start-Direction(
    [string]$Label,
    [string]$Endpoint,
    [uint32]$TransmitId,
    [uint32]$ReceiveId,
    [string]$Directory) {
    Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $Endpoint
    Set-ProcessEnvironment 'UCAL_HARNESS_TX_FRAME_ID' ([string]$TransmitId)
    Set-ProcessEnvironment 'UCAL_HARNESS_RX_FRAME_ID' ([string]$ReceiveId)
    $stdout = Join-Path $Directory "$Label.stdout.jsonl"
    $stderr = Join-Path $Directory "$Label.stderr.log"
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $HarnessPath
    $start.Arguments = 'can-hardware-smoke'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Direction could not be started: $Label" }
    return [ordered]@{
        label = $Label
        process = $process
        stdout = $stdout
        stderr = $stderr
    }
}

$BrokerPath = Resolve-RequiredFile $BrokerPath 'Broker'
$HarnessPath = Resolve-RequiredFile $HarnessPath 'Harness'
$RuntimePath = Resolve-RequiredFile $RuntimePath "$Backend runtime"
if ($EndpointA -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$' -or
    $EndpointB -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$') {
    throw 'Both endpoints must be exact UUIDs from a same-runtime Discover result.'
}
if ($EndpointA -eq $EndpointB) { throw 'EndpointA and EndpointB must be different.' }
if ($Mode -eq 'Execute' -and $backendPolicy.requires_termination -and
    $Termination -notin @('On', 'Off')) {
    throw 'This backend requires explicit -Termination On or -Termination Off.'
}
if ($Mode -eq 'Execute' -and (-not $backendPolicy.submission_correlated) -and $Termination -ne 'Auto') {
    throw 'This backend requires -Termination Auto.'
}
if ((-not $backendPolicy.submission_correlated) -and
    ($MaximumP95LatencyUs -ne 0 -or $MaximumP99LatencyUs -ne 0 -or
     $MaximumP999LatencyUs -ne 0)) {
    throw 'These full-duplex receive sequences are not submission-correlated; use the dedicated latency acceptance instead of applying latency budgets here.'
}
$effectivePayloadLength = if ($PayloadLength -eq 0) { if ($CanFd) { 64 } else { 8 } } else { $PayloadLength }
$validPayloads = if ($CanFd) { @(4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64) } else { @(4, 5, 6, 7, 8) }
if ($effectivePayloadLength -notin $validPayloads) {
    throw 'PayloadLength must map exactly to a valid classic-CAN or CAN-FD DLC.'
}

$plan = [ordered]@{
    schemaVersion = 2
    mode = $Mode
    backend = $Backend
    endpointA = $EndpointA
    endpointB = $EndpointB
    canFd = [bool]$CanFd
    nominalBitrate = $NominalBitrate
    dataBitrate = $DataBitrate
    payloadLength = $effectivePayloadLength
    frameCountPerDirection = $FrameCount
    interFrameDelayUs = $InterFrameDelayUs
    eventPollEveryFrames = $EventPollEveryFrames
    timeoutMs = $TimeoutMs
    startDelayMs = $StartDelayMs
    termination = $Termination
    successEvidence = if ($backendPolicy.requires_tx_completion) {
        'peer-receive-and-correlated-transmit-completion'
    } else {
        'peer-receive; Driver queue acceptance is not physical transmit completion'
    }
    frameIds = [ordered]@{ aToB = 0x5a5; bToA = 0x5a6 }
    latencyBudgetsUs = [ordered]@{
        p95 = if ($MaximumP95LatencyUs -eq 0) { $null } else { $MaximumP95LatencyUs }
        p99 = if ($MaximumP99LatencyUs -eq 0) { $null } else { $MaximumP99LatencyUs }
        p999 = if ($MaximumP999LatencyUs -eq 0) { $null } else { $MaximumP999LatencyUs }
    }
    resourceGrowthBudgets = [ordered]@{
        privateBytes = if ($MaximumPrivateGrowthBytes -eq 0) { $null } else { $MaximumPrivateGrowthBytes }
        handles = if ($MaximumHandleGrowth -eq 0) { $null } else { $MaximumHandleGrowth }
        privateTrendBytesPerMinute = if ($MaximumPrivateTrendBytesPerMinute -eq 0) {
            $null
        } else { $MaximumPrivateTrendBytesPerMinute }
    }
    resourceSampleSeconds = if ($ResourceSampleSeconds -eq 0) { $null } else { $ResourceSampleSeconds }
    artifacts = [ordered]@{
        broker = Get-ArtifactIdentity $BrokerPath
        harness = Get-ArtifactIdentity $HarnessPath
        runtime = Get-ArtifactIdentity $RuntimePath
    }
}
if ($Mode -eq 'Plan') {
    [ordered]@{ status = 'planned'; plan = $plan } | ConvertTo-Json -Depth 8 -Compress
    exit 0
}

$activeBroker = Get-Process -Name 'UcalBroker' -ErrorAction SilentlyContinue
if ($activeBroker) {
    throw 'A UCAL Broker is already running. This runner will not stop or reuse an unowned Broker.'
}
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $EvidenceDirectory = Join-Path $repoRoot `
        "out\hardware-smoke\$stamp-$($Backend.ToLower())-full-duplex"
}
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null

$environmentNames = @(
    'UCAL_BACKEND',
    'UCAL_HARNESS_ENDPOINT_ID',
    'UCAL_HARNESS_CAN_FD', 'UCAL_HARNESS_NBITRATE', 'UCAL_HARNESS_DBITRATE',
    'UCAL_HARNESS_NSP', 'UCAL_HARNESS_DSP', 'UCAL_HARNESS_TERMINATION',
    'UCAL_HARNESS_TIMEOUT_MS', 'UCAL_HARNESS_FRAME_COUNT',
    'UCAL_HARNESS_INTER_FRAME_US', 'UCAL_HARNESS_MAX_P95_LATENCY_US',
    'UCAL_HARNESS_MAX_P99_LATENCY_US', 'UCAL_HARNESS_MAX_P999_LATENCY_US',
    'UCAL_HARNESS_POLL_EVERY_FRAMES', 'UCAL_HARNESS_EMPTY_POLL_DELAY_US',
    'UCAL_HARNESS_PAYLOAD_LENGTH', 'UCAL_HARNESS_LISTEN_ONLY',
    'UCAL_HARNESS_NO_ACK', 'UCAL_HARNESS_EXPECT_NO_RECEIVE',
    'UCAL_HARNESS_EXTENDED_FRAME', 'UCAL_HARNESS_REMOTE_FRAME',
    'UCAL_HARNESS_REQUIRE_RECEIVED', 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED',
    'UCAL_HARNESS_REQUIRE_TX_COMPLETION', 'UCAL_HARNESS_REQUIRE_TIMESTAMP',
    'UCAL_HARNESS_ALLOW_SUBMIT_ONLY', 'UCAL_HARNESS_ALLOW_DUPLICATES',
    'UCAL_HARNESS_MEASURE_RX_LATENCY',
    'UCAL_HARNESS_TX_FRAME_ID', 'UCAL_HARNESS_RX_FRAME_ID',
    'UCAL_HARNESS_START_DELAY_MS'
)
$environmentNames += @(Get-UcalHardwareRuntimeEnvironmentNames $repoRoot)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

$result = [ordered]@{
    schemaVersion = 2
    status = 'error'
    stage = 'preflight'
    startedUtc = [DateTime]::UtcNow.ToString('o')
    completedUtc = $null
    plan = $plan
    endpointCatalog = @()
    directions = $null
    resources = @()
    resourceSamples = @()
    cleanup = [ordered]@{
        shutdownRequested = $false
        shutdownExitCode = $null
        brokerExited = $false
        forced = $false
    }
    error = $null
}
$brokerProcess = $null
$directionA = $null
$directionB = $null
$metrics = @{}
$resourceSamples = [Collections.Generic.List[object]]::new()
$nextResourceSample = [DateTime]::UtcNow
$exitCode = 1

try {
    Set-ProcessEnvironment 'UCAL_BACKEND' $Backend
    Set-ProcessEnvironment `
        $backendPolicy.runtime_environment `
        $RuntimePath
    Set-ProcessEnvironment 'UCAL_HARNESS_CAN_FD' $(if ($CanFd) { '1' } else { '0' })
    Set-ProcessEnvironment 'UCAL_HARNESS_NBITRATE' ([string]$NominalBitrate)
    Set-ProcessEnvironment 'UCAL_HARNESS_DBITRATE' ([string]$DataBitrate)
    Set-ProcessEnvironment 'UCAL_HARNESS_NSP' $null
    Set-ProcessEnvironment 'UCAL_HARNESS_DSP' $null
    Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($Termination) {
        'On' { '1' }
        'Off' { '0' }
        default { $null }
    })
    Set-ProcessEnvironment 'UCAL_HARNESS_TIMEOUT_MS' ([string]$TimeoutMs)
    Set-ProcessEnvironment 'UCAL_HARNESS_FRAME_COUNT' ([string]$FrameCount)
    Set-ProcessEnvironment 'UCAL_HARNESS_INTER_FRAME_US' ([string]$InterFrameDelayUs)
    Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P95_LATENCY_US' ([string]$MaximumP95LatencyUs)
    Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P99_LATENCY_US' ([string]$MaximumP99LatencyUs)
    Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P999_LATENCY_US' ([string]$MaximumP999LatencyUs)
    Set-ProcessEnvironment 'UCAL_HARNESS_POLL_EVERY_FRAMES' ([string]$EventPollEveryFrames)
    Set-ProcessEnvironment 'UCAL_HARNESS_EMPTY_POLL_DELAY_US' '1000'
    Set-ProcessEnvironment 'UCAL_HARNESS_PAYLOAD_LENGTH' ([string]$effectivePayloadLength)
    Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_EXPECT_NO_RECEIVE' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_EXTENDED_FRAME' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_REMOTE_FRAME' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_RECEIVED' '1'
    Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TX_COMPLETION' `
        $(if ($backendPolicy.requires_tx_completion) { '1' } else { '0' })
    Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TIMESTAMP' '1'
    Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_SUBMIT_ONLY' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_DUPLICATES' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_MEASURE_RX_LATENCY' `
        $(if ((-not $backendPolicy.submission_correlated)) { '0' } else { '1' })
    Set-ProcessEnvironment 'UCAL_HARNESS_START_DELAY_MS' ([string]$StartDelayMs)

    $result.stage = 'broker-start'
    $brokerProcess = Start-Process -FilePath $BrokerPath -ArgumentList '--allow-test-shutdown' `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $EvidenceDirectory 'broker.stdout.jsonl') `
        -RedirectStandardError (Join-Path $EvidenceDirectory 'broker.stderr.log')
    $ready = $false
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        & $HarnessPath ping *> $null
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw 'Owned Broker did not become ready within five seconds.' }

    $result.stage = 'discovery'
    $catalogLines = @(& $HarnessPath backend-endpoints)
    if ($LASTEXITCODE -ne 0) { throw 'Endpoint discovery failed.' }
    $result.endpointCatalog = @($catalogLines | ForEach-Object { $_ | ConvertFrom-Json })
    foreach ($endpoint in @($EndpointA, $EndpointB)) {
        $selected = @($result.endpointCatalog | Where-Object { $_.endpoint -eq $endpoint })
        if ($selected.Count -ne 1 -or [uint32]$selected[0].busType -ne 1 -or
            [uint32]$selected[0].state -ne 0) {
            throw "Endpoint is not one available CAN endpoint: $endpoint"
        }
        $capabilities = [uint32]$selected[0].capabilities
        if (($backendPolicy.requires_tx_completion -and ($capabilities -band 0x200) -eq 0) -or
            ($backendPolicy.requires_termination -and ($capabilities -band 0x80) -eq 0)) {
            throw "Endpoint lacks required completion or termination capability: $endpoint"
        }
        if ($CanFd -and (($capabilities -band 0x6) -ne 0x6)) {
            throw "Endpoint lacks ISO CAN FD/BRS capability: $endpoint"
        }
    }

    $result.stage = 'full-duplex'
    $directionA = Start-Direction 'a-to-b' $EndpointA 0x5a5 0x5a6 $EvidenceDirectory
    $directionB = Start-Direction 'b-to-a' $EndpointB 0x5a6 0x5a5 $EvidenceDirectory
    $processA = [Diagnostics.Process]$directionA['process']
    $processB = [Diagnostics.Process]$directionB['process']
    $pacedTransmitBudgetMs = [int64][Math]::Ceiling(
        ([double]$FrameCount * [double]$InterFrameDelayUs) / 1000.0)
    # The harness enforces TimeoutMs on its own final receive drain. This outer
    # watchdog must additionally allow per-frame submit/poll/scheduler overhead;
    # otherwise a declared 30-minute paced run can be killed a few seconds before
    # the harness reports its bounded result.
    $watchdogProcessingSlackMs = [int64][Math]::Max(
        30000.0, [Math]::Ceiling([double]$pacedTransmitBudgetMs * 0.25))
    $deadline = [DateTime]::UtcNow.AddMilliseconds(
        [int64]$TimeoutMs + [int64]$StartDelayMs + $watchdogProcessingSlackMs +
        $pacedTransmitBudgetMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($process in @($brokerProcess, $processA, $processB)) {
            Update-ProcessMetrics $process $metrics
        }
        if ($ResourceSampleSeconds -ne 0 -and [DateTime]::UtcNow -ge $nextResourceSample) {
            foreach ($process in @($brokerProcess, $processA, $processB)) {
                try {
                    $process.Refresh()
                    if (-not $process.HasExited) {
                        $resourceSamples.Add([pscustomobject][ordered]@{
                            utc = [DateTime]::UtcNow.ToString('o')
                            pid = $process.Id
                            name = $process.ProcessName
                            workingSetBytes = [int64]$process.WorkingSet64
                            privateBytes = [int64]$process.PrivateMemorySize64
                            handleCount = [int]$process.HandleCount
                            threadCount = [int]$process.Threads.Count
                        })
                    }
                } catch {
                    # The final sample may race a normally exiting direction.
                }
            }
            $nextResourceSample = [DateTime]::UtcNow.AddSeconds($ResourceSampleSeconds)
        }
        $processA.Refresh()
        $processB.Refresh()
        if ($processA.HasExited -and $processB.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    $processA.Refresh()
    $processB.Refresh()
    if (-not $processA.HasExited -or -not $processB.HasExited) {
        throw 'Full-duplex directions did not finish within the bounded timeout.'
    }
    $processA.WaitForExit()
    $processB.WaitForExit()
    $aExitCode = $processA.ExitCode
    $bExitCode = $processB.ExitCode
    $aText = $processA.StandardOutput.ReadToEnd()
    $bText = $processB.StandardOutput.ReadToEnd()
    $aStderr = $processA.StandardError.ReadToEnd()
    $bStderr = $processB.StandardError.ReadToEnd()
    [IO.File]::WriteAllText($directionA.stdout, $aText)
    [IO.File]::WriteAllText($directionB.stdout, $bText)
    [IO.File]::WriteAllText($directionA.stderr, $aStderr)
    [IO.File]::WriteAllText($directionB.stderr, $bStderr)
    $aEvents = @(ConvertFrom-JsonLines $aText)
    $bEvents = @(ConvertFrom-JsonLines $bText)
    $result.directions = [ordered]@{
        aToB = [ordered]@{
            exitCode = $aExitCode
            events = $aEvents
            stderr = $aStderr
        }
        bToA = [ordered]@{
            exitCode = $bExitCode
            events = $bEvents
            stderr = $bStderr
        }
    }
    if ($aExitCode -ne 0 -or $bExitCode -ne 0) {
        throw "Full-duplex assertions failed: A=$aExitCode, B=$bExitCode."
    }
    $aSummary = @($aEvents | Where-Object { $null -ne $_.canHardwareSmoke })[-1]
    $bSummary = @($bEvents | Where-Object { $null -ne $_.canHardwareSmoke })[-1]
    $requireCompletion = $backendPolicy.requires_tx_completion
    if (-not $aSummary.canHardwareSmoke -or -not $bSummary.canHardwareSmoke -or
        $aSummary.submittedCount -ne $FrameCount -or $aSummary.receivedCount -ne $FrameCount -or
        $bSummary.submittedCount -ne $FrameCount -or $bSummary.receivedCount -ne $FrameCount -or
        ($requireCompletion -and ($aSummary.txCompletedCount -ne $FrameCount -or
                                  $bSummary.txCompletedCount -ne $FrameCount)) -or
        (-not $requireCompletion -and ($aSummary.txCompletedCount -ne 0 -or
                                       $bSummary.txCompletedCount -ne 0))) {
        throw 'Full-duplex count or evidence assertions failed.'
    }
    $result.resources = @($metrics.Values)
    $result.resourceSamples = @($resourceSamples)
    foreach ($resource in $result.resources) {
        $privateGrowth = [int64]$resource.lastPrivateBytes - [int64]$resource.firstPrivateBytes
        $handleGrowth = [int]$resource.lastHandleCount - [int]$resource.firstHandleCount
        $resource.privateGrowthBytes = $privateGrowth
        $resource.handleGrowth = $handleGrowth
        $processSamples = @($resourceSamples | Where-Object { $_.pid -eq $resource.pid })
        $privateTrend = 0.0
        if ($processSamples.Count -ge 2) {
            $origin = [DateTime]::Parse([string]$processSamples[0].utc).ToUniversalTime()
            $points = @($processSamples | ForEach-Object {
                [pscustomobject]@{
                    x = ([DateTime]::Parse([string]$_.utc).ToUniversalTime() - $origin).TotalMinutes
                    y = [double]$_.privateBytes
                }
            })
            $meanX = ($points | Measure-Object -Property x -Average).Average
            $meanY = ($points | Measure-Object -Property y -Average).Average
            $numerator = 0.0
            $denominator = 0.0
            foreach ($point in $points) {
                $deltaX = [double]$point.x - [double]$meanX
                $numerator += $deltaX * ([double]$point.y - [double]$meanY)
                $denominator += $deltaX * $deltaX
            }
            if ($denominator -gt 0.0) { $privateTrend = $numerator / $denominator }
        }
        $resource.privateTrendBytesPerMinute = [int64][Math]::Round($privateTrend)
        if ($MaximumPrivateGrowthBytes -ne 0 -and
            $privateGrowth -gt $MaximumPrivateGrowthBytes) {
            throw "$($resource.name) private-memory growth exceeded the bounded gate."
        }
        if ($MaximumHandleGrowth -ne 0 -and $handleGrowth -gt $MaximumHandleGrowth) {
            throw "$($resource.name) handle growth exceeded the bounded gate."
        }
        if ($MaximumPrivateTrendBytesPerMinute -ne 0 -and
            $privateTrend -gt $MaximumPrivateTrendBytesPerMinute) {
            throw "$($resource.name) private-memory trend exceeded the bounded gate."
        }
    }
    $result.status = 'passed'
    $result.stage = 'complete'
    $exitCode = 0
} catch {
    $result.error = $_.Exception.Message
} finally {
    foreach ($direction in @($directionA, $directionB)) {
        if ($null -ne $direction -and $null -ne $direction['process']) {
            $ownedProcess = [Diagnostics.Process]$direction['process']
            $ownedProcess.Refresh()
            if (-not $ownedProcess.HasExited) {
                Stop-Process -Id $ownedProcess.Id -Force -ErrorAction SilentlyContinue
                $ownedProcess.WaitForExit()
            }
        }
    }
    if ($null -ne $brokerProcess) {
        $brokerProcess.Refresh()
        if (-not $brokerProcess.HasExited) {
            $result.cleanup.shutdownRequested = $true
            & $HarnessPath shutdown *> (Join-Path $EvidenceDirectory 'shutdown.log')
            $result.cleanup.shutdownExitCode = $LASTEXITCODE
            if (-not $brokerProcess.WaitForExit(5000)) {
                Stop-Process -Id $brokerProcess.Id -Force -ErrorAction SilentlyContinue
                $brokerProcess.WaitForExit()
                $result.cleanup.forced = $true
            }
        }
        $brokerProcess.Refresh()
        $result.cleanup.brokerExited = $brokerProcess.HasExited
    }
    foreach ($name in $environmentNames) {
        Set-ProcessEnvironment $name $savedEnvironment[$name]
    }
    if ($result.resources.Count -eq 0) { $result.resources = @($metrics.Values) }
    if ($result.resourceSamples.Count -eq 0) { $result.resourceSamples = @($resourceSamples) }
    $result.completedUtc = [DateTime]::UtcNow.ToString('o')
    $result.output = [ordered]@{
        directory = $EvidenceDirectory
        manifest = Join-Path $EvidenceDirectory 'result.json'
    }
    $result | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $result.output.manifest -Encoding UTF8
    $result | ConvertTo-Json -Depth 14 -Compress
}
exit $exitCode
