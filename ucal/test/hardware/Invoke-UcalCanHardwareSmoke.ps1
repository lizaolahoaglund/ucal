[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Backend,

    [ValidateSet('Plan', 'Discover', 'Execute')]
    [string]$Mode = 'Plan',

    [ValidateSet('x64', 'Win32')]
    [string]$Architecture = 'x64',

    [string]$EndpointId,

    [string]$PeerEndpointId,

    [ValidateSet('Receive', 'SelfReceive', 'TxCompletion', 'ReceiveAndTxCompletion', 'RejectReceive')]
    [string]$EvidenceMode = 'Receive',

    [ValidateRange(1000, 10000000)]
    [uint32]$NominalBitrate = 500000,

    [ValidateRange(1000, 12000000)]
    [uint32]$DataBitrate = 2000000,

    [ValidateRange(0, 99)]
    [uint32]$NominalSamplePoint = 0,

    [ValidateRange(0, 99)]
    [uint32]$DataSamplePoint = 0,

    [switch]$CanFd,

    [ValidateSet('Auto', 'On', 'Off')]
    [string]$Termination = 'Auto',

    [ValidateRange(100, 60000)]
    [uint32]$TimeoutMs = 3000,

    [bool]$RequireTimestamp = $true,

    [ValidateRange(1, 100000)]
    [uint32]$FrameCount = 1,

    [ValidateRange(0, 1000000)]
    [uint32]$InterFrameDelayUs = 0,

    [ValidateRange(0, 60000)]
    [uint32]$PostSubmitHoldMs = 0,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP95LatencyUs = 0,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP99LatencyUs = 0,

    [ValidateRange(0, 60000000)]
    [uint32]$MaximumP999LatencyUs = 0,

    [ValidateRange(1, 10000)]
    [uint32]$EventPollEveryFrames = 1,

    [ValidateRange(0, 1000000)]
    [uint32]$EmptyPollDelayUs = 1000,

    [ValidateRange(0, 1000000)]
    [uint32]$PeerEmptyPollDelayUs = 5000,

    [ValidateRange(0, 64)]
    [uint32]$PayloadLength = 0,

    [switch]$ZeroLengthPayload,

    [switch]$ConfigurationOnly,

    [switch]$ListenOnly,

    [switch]$NoAck,

    [switch]$PeerListenOnly,

    [switch]$PeerNoAck,

    [switch]$PeerAllowDuplicates,

    [switch]$PeerLatency,

    [switch]$PeerErrorRecovery,

    [ValidateSet('Match', 'Auto', 'On', 'Off')]
    [string]$PeerTermination = 'Match',

    [switch]$ExtendedFrame,

    [switch]$RemoteFrame,

    [ValidateRange(0, 8)]
    [uint32]$RemoteDlc = 8,

    [switch]$StandardFilter,

    [uint32]$StandardFilterCode = 0,

    [uint32]$StandardFilterMask = 0,

    [switch]$ExtendedFilter,

    [uint32]$ExtendedFilterCode = 0,

    [uint32]$ExtendedFilterMask = 0,

    [string]$BrokerPath,
    [string]$HarnessPath,
    [string]$RuntimePath,
    [string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($BrokerPath)) {
    $BrokerPath = Join-Path $repoRoot "out\$Architecture\Release\UcalBroker.exe"
}
if ([string]::IsNullOrWhiteSpace($HarnessPath)) {
    $HarnessPath = Join-Path $repoRoot "out\$Architecture\Release\UcalHarness.exe"
}

function Resolve-RequiredFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-ArtifactIdentity([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $resolved = Resolve-RequiredFile $Path 'Runtime'
    return [ordered]@{
        path = $resolved
        sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    }
}

function Set-ProcessEnvironment([string]$Name, [AllowNull()][string]$Value) {
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}

function Invoke-CapturedProcess(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$StdoutPath,
    [string]$StderrPath) {
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
        -PassThru -Wait -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath
    return [ordered]@{
        exitCode = $process.ExitCode
        stdout = if (Test-Path -LiteralPath $StdoutPath) {
            [IO.File]::ReadAllText($StdoutPath)
        } else { '' }
        stderr = if (Test-Path -LiteralPath $StderrPath) {
            [IO.File]::ReadAllText($StderrPath)
        } else { '' }
    }
}

function ConvertFrom-JsonLines([string]$Text) {
    $items = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $items += $line | ConvertFrom-Json
    }
    return @($items)
}

$BrokerPath = Resolve-RequiredFile $BrokerPath 'Broker'
$HarnessPath = Resolve-RequiredFile $HarnessPath 'Harness'
$runtimeIdentity = Get-ArtifactIdentity $RuntimePath
Import-Module (Join-Path $PSScriptRoot 'Hardware.psm1') -Force
$backendPolicy = Get-UcalHardwareTestProfile $repoRoot $Backend
$brokerIdentity = [ordered]@{
    path = $BrokerPath
    sha256 = (Get-FileHash -LiteralPath $BrokerPath -Algorithm SHA256).Hash
}
$harnessIdentity = [ordered]@{
    path = $HarnessPath
    sha256 = (Get-FileHash -LiteralPath $HarnessPath -Algorithm SHA256).Hash
}

$expectNoReceive = -not $ConfigurationOnly -and $EvidenceMode -eq 'RejectReceive'
$requireReceived = -not $ConfigurationOnly -and $EvidenceMode -in @('Receive', 'SelfReceive', 'ReceiveAndTxCompletion')
$requireSelfReceived = -not $ConfigurationOnly -and $EvidenceMode -eq 'SelfReceive'
$requireTxCompletion = -not $ConfigurationOnly -and
    ($EvidenceMode -in @('TxCompletion', 'ReceiveAndTxCompletion') -or
     ($EvidenceMode -eq 'RejectReceive' -and $backendPolicy.filter_rejection_requires_self_receive))
$timingVerification = if ($backendPolicy.timing_readback -eq 'exact-public-runtime-readback') {
    [ordered]@{
        level = 'exact-public-runtime-readback'
        nominal = 'required'
        data = if ($CanFd) { 'required' } else { 'not-applicable' }
    }
} else {
    [ordered]@{
        level = 'driver-configuration-call-accepted'
        nominal = 'accepted-not-read-back'
        data = if ($CanFd) { 'accepted-not-read-back' } else { 'not-applicable' }
    }
}

$plan = [ordered]@{
    schemaVersion = 2
    mode = $Mode
    backend = $Backend
    architecture = $Architecture
    endpointId = $EndpointId
    peerEndpointId = $PeerEndpointId
    can = [ordered]@{
        canFd = [bool]$CanFd
        nominalBitrate = $NominalBitrate
        dataBitrate = $DataBitrate
        nominalSamplePoint = if ($NominalSamplePoint -eq 0) { $null } else { $NominalSamplePoint }
        dataSamplePoint = if ($DataSamplePoint -eq 0) { $null } else { $DataSamplePoint }
        termination = $Termination
        timeoutMs = $TimeoutMs
        frameCount = $FrameCount
        interFrameDelayUs = $InterFrameDelayUs
        postSubmitHoldMs = $PostSubmitHoldMs
        maximumP95LatencyUs = if ($MaximumP95LatencyUs -eq 0) { $null } else { $MaximumP95LatencyUs }
        maximumP99LatencyUs = if ($MaximumP99LatencyUs -eq 0) { $null } else { $MaximumP99LatencyUs }
        maximumP999LatencyUs = if ($MaximumP999LatencyUs -eq 0) { $null } else { $MaximumP999LatencyUs }
        eventPollEveryFrames = $EventPollEveryFrames
        emptyPollDelayUs = $EmptyPollDelayUs
        peerEmptyPollDelayUs = $PeerEmptyPollDelayUs
        payloadLength = if ($ZeroLengthPayload) { 0 } elseif ($PayloadLength -eq 0) {
            $null
        } else { $PayloadLength }
        configurationOnly = [bool]$ConfigurationOnly
        listenOnly = [bool]$ListenOnly
        noAck = [bool]$NoAck
        peerListenOnly = [bool]$PeerListenOnly
        peerNoAck = [bool]$PeerNoAck
        peerAllowDuplicates = [bool]$PeerAllowDuplicates
        peerLatency = [bool]$PeerLatency
        peerErrorRecovery = [bool]$PeerErrorRecovery
        peerTermination = $PeerTermination
        extendedFrame = [bool]$ExtendedFrame
        remoteFrame = [bool]$RemoteFrame
        remoteDlc = if ($RemoteFrame) { $RemoteDlc } else { $null }
        standardFilter = if ($StandardFilter) { [ordered]@{ code = $StandardFilterCode; mask = $StandardFilterMask } } else { $null }
        extendedFilter = if ($ExtendedFilter) { [ordered]@{ code = $ExtendedFilterCode; mask = $ExtendedFilterMask } } else { $null }
        timingVerification = $timingVerification
    }
    requirements = [ordered]@{
        received = $requireReceived
        selfReceived = $requireSelfReceived
        txCompletion = $requireTxCompletion
        timestamp = $RequireTimestamp
        noReceive = $expectNoReceive
    }
    artifacts = [ordered]@{
        broker = $brokerIdentity
        harness = $harnessIdentity
        runtime = $runtimeIdentity
    }
}

if ($Mode -eq 'Plan') {
    [ordered]@{ status = 'planned'; plan = $plan } | ConvertTo-Json -Depth 8 -Compress
    exit 0
}

if ($Mode -eq 'Execute' -and
    $EndpointId -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$') {
    throw 'Execute mode requires an exact endpoint UUID from a same-runtime Discover result.'
}
if ($Mode -eq 'Execute' -and -not [string]::IsNullOrWhiteSpace($PeerEndpointId) -and
    $PeerEndpointId -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$') {
    throw 'PeerEndpointId must be an exact endpoint UUID from the same Discover result.'
}
if ($Mode -eq 'Execute' -and $PeerEndpointId -eq $EndpointId) {
    throw 'PeerEndpointId must identify a different physical endpoint.'
}
if ($ConfigurationOnly -and -not [string]::IsNullOrWhiteSpace($PeerEndpointId)) {
    throw 'ConfigurationOnly accepts one endpoint and cannot be combined with PeerEndpointId.'
}
if ($ListenOnly -and $NoAck) {
    throw 'ListenOnly and NoAck are mutually exclusive operating modes.'
}
if ($ZeroLengthPayload -and $PayloadLength -ne 0) {
    throw 'ZeroLengthPayload cannot be combined with a nonzero PayloadLength.'
}
if ($ZeroLengthPayload -or $PayloadLength -ne 0) {
    $effectivePayloadLength = if ($ZeroLengthPayload) { 0 } else { $PayloadLength }
    $validPayloads = if ($CanFd) { @(0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64) } else { @(0, 1, 2, 3, 4, 5, 6, 7, 8) }
    if ($effectivePayloadLength -notin $validPayloads) {
        throw 'PayloadLength must map exactly to a valid classic-CAN or CAN-FD DLC.'
    }
    if ($effectivePayloadLength -lt 4 -and $FrameCount -ne 1) {
        throw 'Payload lengths below four bytes require FrameCount 1 because no sequence marker fits.'
    }
}
if ($RemoteFrame -and
    ($CanFd -or $FrameCount -ne 1 -or $PayloadLength -ne 0 -or $ZeroLengthPayload)) {
    throw 'RemoteFrame is classic-CAN only, requires FrameCount 1, and cannot be combined with PayloadLength.'
}
if (-not [string]::IsNullOrWhiteSpace($PeerEndpointId) -and
    $EvidenceMode -notin @('Receive', 'SelfReceive', 'ReceiveAndTxCompletion', 'RejectReceive')) {
    throw 'PeerEndpointId requires Receive, SelfReceive, ReceiveAndTxCompletion, or RejectReceive evidence.'
}
if ($EvidenceMode -eq 'RejectReceive' -and [string]::IsNullOrWhiteSpace($PeerEndpointId)) {
    throw 'RejectReceive requires an independently opened peer endpoint.'
}
if ($PeerLatency -and [string]::IsNullOrWhiteSpace($PeerEndpointId)) {
    throw 'PeerLatency requires an independently opened peer endpoint.'
}
if ($PeerLatency -and
    ($EvidenceMode -ne 'Receive' -or $ConfigurationOnly -or $ListenOnly -or $NoAck -or
     $PeerListenOnly -or $PeerNoAck -or $PeerAllowDuplicates -or $ExtendedFrame -or
     $RemoteFrame -or $StandardFilter -or $ExtendedFilter)) {
    throw 'PeerLatency requires normal standard-frame Receive mode without filters or operating-mode overrides.'
}
if ($PeerLatency -and $PayloadLength -ne 0 -and
    (($CanFd -and $PayloadLength -ne 64) -or (-not $CanFd -and $PayloadLength -ne 8))) {
    throw 'PeerLatency supports an 8-byte Classic CAN payload or a 64-byte CAN FD payload.'
}
if ($PeerLatency -and $ZeroLengthPayload) {
    throw 'PeerLatency does not support a zero-length payload.'
}
if ($PeerErrorRecovery -and [string]::IsNullOrWhiteSpace($PeerEndpointId)) {
    throw 'PeerErrorRecovery requires an independently opened peer endpoint.'
}
if ($PeerErrorRecovery -and
    ($EvidenceMode -ne 'Receive' -or $ConfigurationOnly -or $CanFd -or $ListenOnly -or
     $NoAck -or $PeerListenOnly -or $PeerNoAck -or $PeerAllowDuplicates -or
     $PeerLatency -or $ExtendedFrame -or $RemoteFrame -or $StandardFilter -or
     $ExtendedFilter)) {
    throw 'PeerErrorRecovery requires normal Classic CAN Receive mode without filters or operating-mode overrides.'
}
if ($Mode -eq 'Execute' -and $backendPolicy.requires_termination -and $Termination -eq 'Auto') {
    throw 'This backend requires explicit -Termination On or -Termination Off.'
}
if ($Mode -eq 'Execute' -and (-not $backendPolicy.supports_termination) -and $Termination -ne 'Auto') {
    throw 'This backend requires -Termination Auto unless a future endpoint advertises programmable termination.'
}
if ($PeerListenOnly -and $PeerNoAck) {
    throw 'PeerListenOnly and PeerNoAck are mutually exclusive operating modes.'
}
if ([string]::IsNullOrWhiteSpace($PeerEndpointId) -and
    ($PeerListenOnly -or $PeerNoAck -or $PeerAllowDuplicates -or
     $PeerTermination -ne 'Match')) {
    throw 'Peer operating-mode overrides require PeerEndpointId.'
}
if ($Mode -eq 'Execute' -and (-not $backendPolicy.supports_termination) -and
    $PeerTermination -notin @('Match', 'Auto')) {
    throw 'This backend requires automatic termination handling.'
}
if ($Mode -eq 'Execute' -and (-not $backendPolicy.supports_tx_completion) -and $requireTxCompletion) {
    throw "$Backend does not advertise transaction-correlated Tx completion; use physical receive evidence."
}

$activeBroker = if (-not $env:UCAL_BROKER_INSTANCE) { Get-Process -Name 'UcalBroker' -ErrorAction SilentlyContinue } else { $null }
if ($activeBroker) {
    throw 'A UCAL Broker is already running. This runner will not stop or reuse an unowned Broker.'
}

if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $EvidenceDirectory = Join-Path $repoRoot "out\hardware-smoke\$stamp-$($Backend.ToLower())-$($Mode.ToLower())"
}
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null

$environmentNames = @(
    'UCAL_BACKEND',
    'UCAL_HARNESS_ENDPOINT_ID', 'UCAL_HARNESS_PEER_ENDPOINT_ID',
    'UCAL_HARNESS_CAN_FD',
    'UCAL_HARNESS_NBITRATE', 'UCAL_HARNESS_DBITRATE',
    'UCAL_HARNESS_NSP', 'UCAL_HARNESS_DSP',
    'UCAL_HARNESS_TERMINATION', 'UCAL_HARNESS_TIMEOUT_MS',
    'UCAL_HARNESS_FRAME_COUNT', 'UCAL_HARNESS_INTER_FRAME_US', 'UCAL_HARNESS_POST_SUBMIT_HOLD_MS',
    'UCAL_HARNESS_MAX_P95_LATENCY_US', 'UCAL_HARNESS_POLL_EVERY_FRAMES',
    'UCAL_HARNESS_MAX_P99_LATENCY_US', 'UCAL_HARNESS_MAX_P999_LATENCY_US',
    'UCAL_HARNESS_EMPTY_POLL_DELAY_US',
    'UCAL_HARNESS_PAYLOAD_LENGTH',
    'UCAL_HARNESS_LISTEN_ONLY', 'UCAL_HARNESS_NO_ACK',
    'UCAL_HARNESS_EXPECT_NO_RECEIVE', 'UCAL_HARNESS_EXTENDED_FRAME',
    'UCAL_HARNESS_REMOTE_FRAME', 'UCAL_HARNESS_REMOTE_DLC',
    'UCAL_HARNESS_STD_FILTER_CODE', 'UCAL_HARNESS_STD_FILTER_MASK',
    'UCAL_HARNESS_EXT_FILTER_CODE', 'UCAL_HARNESS_EXT_FILTER_MASK',
    'UCAL_HARNESS_REQUIRE_RECEIVED', 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED',
    'UCAL_HARNESS_REQUIRE_TX_COMPLETION', 'UCAL_HARNESS_REQUIRE_TIMESTAMP',
    'UCAL_HARNESS_ALLOW_SUBMIT_ONLY', 'UCAL_HARNESS_ALLOW_DUPLICATES'
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
    harness = $null
    timingVerification = [ordered]@{
        level = $timingVerification.level
        attempted = $false
        passed = $null
    }
    cleanup = [ordered]@{
        shutdownRequested = $false
        shutdownExitCode = $null
        brokerExited = $false
        forced = $false
    }
    error = $null
}
$brokerProcess = $null
$peerProcess = $null
$exitCode = 1

try {
    Set-ProcessEnvironment 'UCAL_BACKEND' $Backend
    Set-ProcessEnvironment 'UCAL_HARNESS_POST_SUBMIT_HOLD_MS' ([string]$PostSubmitHoldMs)
    if ($runtimeIdentity) {
        $runtimeVariable = $backendPolicy.runtime_environment
        Set-ProcessEnvironment $runtimeVariable $runtimeIdentity.path
    }
    if ($backendPolicy.echo_environment) {
        Set-ProcessEnvironment $backendPolicy.echo_environment $(if ($requireSelfReceived) { '1' } else { '0' })
    }

    if ($Mode -eq 'Execute') {
        Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $EndpointId
        Set-ProcessEnvironment 'UCAL_HARNESS_PEER_ENDPOINT_ID' `
            $(if ($PeerLatency) { $PeerEndpointId } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_CAN_FD' $(if ($CanFd) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_NBITRATE' ([string]$NominalBitrate)
        Set-ProcessEnvironment 'UCAL_HARNESS_DBITRATE' ([string]$DataBitrate)
        Set-ProcessEnvironment 'UCAL_HARNESS_NSP' $(if ($NominalSamplePoint -eq 0) { $null } else { [string]$NominalSamplePoint })
        Set-ProcessEnvironment 'UCAL_HARNESS_DSP' $(if ($DataSamplePoint -eq 0) { $null } else { [string]$DataSamplePoint })
        Set-ProcessEnvironment 'UCAL_HARNESS_TIMEOUT_MS' ([string]$TimeoutMs)
        Set-ProcessEnvironment 'UCAL_HARNESS_FRAME_COUNT' ([string]$FrameCount)
        Set-ProcessEnvironment 'UCAL_HARNESS_INTER_FRAME_US' ([string]$InterFrameDelayUs)
        Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P95_LATENCY_US' ([string]$MaximumP95LatencyUs)
        Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P99_LATENCY_US' ([string]$MaximumP99LatencyUs)
        Set-ProcessEnvironment 'UCAL_HARNESS_MAX_P999_LATENCY_US' ([string]$MaximumP999LatencyUs)
        Set-ProcessEnvironment 'UCAL_HARNESS_POLL_EVERY_FRAMES' ([string]$EventPollEveryFrames)
        Set-ProcessEnvironment 'UCAL_HARNESS_EMPTY_POLL_DELAY_US' ([string]$EmptyPollDelayUs)
        Set-ProcessEnvironment 'UCAL_HARNESS_PAYLOAD_LENGTH' $(if ($ZeroLengthPayload) {
            '0'
        } elseif ($PayloadLength -eq 0) { $null } else { [string]$PayloadLength })
        Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' $(if ($ListenOnly) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' $(if ($NoAck) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_EXPECT_NO_RECEIVE' $(if ($expectNoReceive) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_EXTENDED_FRAME' $(if ($ExtendedFrame) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_REMOTE_FRAME' $(if ($RemoteFrame) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_REMOTE_DLC' $(if ($RemoteFrame) { [string]$RemoteDlc } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_STD_FILTER_CODE' $(if ($StandardFilter) { [string]$StandardFilterCode } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_STD_FILTER_MASK' $(if ($StandardFilter) { [string]$StandardFilterMask } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_EXT_FILTER_CODE' $(if ($ExtendedFilter) { [string]$ExtendedFilterCode } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_EXT_FILTER_MASK' $(if ($ExtendedFilter) { [string]$ExtendedFilterMask } else { $null })
        Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_RECEIVED' $(if ($requireReceived) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED' $(if ($requireSelfReceived) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TX_COMPLETION' $(if ($requireTxCompletion) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TIMESTAMP' $(if ($RequireTimestamp) { '1' } else { '0' })
        Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_SUBMIT_ONLY' '0'
        Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_DUPLICATES' '0'
        Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($Termination) {
            'On' { '1' }
            'Off' { '0' }
            default { $null }
        })
    }

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
    $catalogRun = Invoke-CapturedProcess $HarnessPath @('backend-endpoints') `
        (Join-Path $EvidenceDirectory 'endpoints.stdout.jsonl') `
        (Join-Path $EvidenceDirectory 'endpoints.stderr.log')
    if ($catalogRun.exitCode -ne 0) {
        throw "Endpoint discovery failed with exit code $($catalogRun.exitCode)."
    }
    $result.endpointCatalog = @(ConvertFrom-JsonLines $catalogRun.stdout)

    if ($Mode -eq 'Discover') {
        $result.status = 'discovered'
        $result.stage = 'complete'
        $exitCode = 0
    } else {
        $selected = @($result.endpointCatalog | Where-Object { $_.endpoint -eq $EndpointId })
        if ($selected.Count -ne 1) {
            throw 'The exact endpoint ID was not present exactly once in this live catalog.'
        }
        if ([uint32]$selected[0].busType -ne 1 -or [uint32]$selected[0].state -ne 0) {
            throw 'The selected endpoint is not an available CAN endpoint.'
        }
        $capabilities = [uint32]$selected[0].capabilities
        if ($CanFd -and (($capabilities -band 0x6) -ne 0x6)) {
            throw 'The selected endpoint does not advertise ISO CAN FD with BRS.'
        }
        if ($requireTxCompletion -and (($capabilities -band 0x200) -eq 0)) {
            throw 'The selected endpoint does not advertise transaction-correlated Tx completion.'
        }
        if ($Termination -ne 'Auto' -and (($capabilities -band 0x80) -eq 0)) {
            throw 'The selected endpoint does not advertise programmable termination.'
        }
        if ($ListenOnly -and (($capabilities -band 0x40) -eq 0)) {
            throw 'The selected endpoint does not advertise listen-only mode.'
        }
        if ($NoAck -and (($capabilities -band 0x100) -eq 0)) {
            throw 'The selected endpoint does not advertise no-ACK mode.'
        }

        $peer = @()
        if (-not [string]::IsNullOrWhiteSpace($PeerEndpointId)) {
            $peer = @($result.endpointCatalog | Where-Object { $_.endpoint -eq $PeerEndpointId })
            if ($peer.Count -ne 1) {
                throw 'The exact peer endpoint ID was not present exactly once in this live catalog.'
            }
            if ([uint32]$peer[0].busType -ne 1 -or [uint32]$peer[0].state -ne 0) {
                throw 'The selected peer is not an available CAN endpoint.'
            }
            $peerCapabilities = [uint32]$peer[0].capabilities
            if ($CanFd -and (($peerCapabilities -band 0x6) -ne 0x6)) {
                throw 'The selected peer does not advertise ISO CAN FD with BRS.'
            }
            if ($Termination -ne 'Auto' -and (($peerCapabilities -band 0x80) -eq 0)) {
                throw 'The selected peer does not advertise programmable termination.'
            }
            if (($PeerListenOnly -or $PeerErrorRecovery) -and
                (($peerCapabilities -band 0x40) -eq 0)) {
                throw 'The selected peer does not advertise listen-only mode.'
            }
            if ($PeerNoAck -and (($peerCapabilities -band 0x100) -eq 0)) {
                throw 'The selected peer does not advertise no-ACK mode.'
            }
            $effectivePeerTermination = if ($PeerTermination -eq 'Match') {
                $Termination
            } else {
                $PeerTermination
            }
            if ($effectivePeerTermination -ne 'Auto' -and
                (($peerCapabilities -band 0x80) -eq 0)) {
                throw 'The selected peer does not advertise the requested programmable termination.'
            }
        }

        $result.stage = 'smoke'
        $result.timingVerification.attempted = $true
        if ($PeerLatency) {
            Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $EndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_PEER_ENDPOINT_ID' $PeerEndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($Termination) {
                'On' { '1' }
                'Off' { '0' }
                default { $null }
            })
            $latencyRun = Invoke-CapturedProcess $HarnessPath @('can-peer-latency') `
                (Join-Path $EvidenceDirectory 'latency.stdout.jsonl') `
                (Join-Path $EvidenceDirectory 'latency.stderr.log')
            $result.harness = [ordered]@{
                exitCode = $latencyRun.exitCode
                events = @(ConvertFrom-JsonLines $latencyRun.stdout)
                stderr = $latencyRun.stderr
            }
            if ($latencyRun.exitCode -ne 0) {
                throw "CAN peer latency assertions failed with exit code $($latencyRun.exitCode)."
            }
        } elseif ($PeerErrorRecovery) {
            Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $EndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_PEER_ENDPOINT_ID' $PeerEndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($Termination) {
                'On' { '1' }
                'Off' { '0' }
                default { $null }
            })
            $recoveryRun = Invoke-CapturedProcess $HarnessPath @('can-peer-error-recovery') `
                (Join-Path $EvidenceDirectory 'error-recovery.stdout.jsonl') `
                (Join-Path $EvidenceDirectory 'error-recovery.stderr.log')
            $result.harness = [ordered]@{
                exitCode = $recoveryRun.exitCode
                events = @(ConvertFrom-JsonLines $recoveryRun.stdout)
                stderr = $recoveryRun.stderr
            }
            if ($recoveryRun.exitCode -ne 0) {
                throw "CAN peer error/recovery assertions failed with exit code $($recoveryRun.exitCode)."
            }
        } elseif ($peer.Count -eq 1) {
            Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $PeerEndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' $(if ($PeerListenOnly) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' $(if ($PeerNoAck) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($effectivePeerTermination) {
                'On' { '1' }
                'Off' { '0' }
                default { $null }
            })
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_RECEIVED' '1'
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TX_COMPLETION' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_SUBMIT_ONLY' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_DUPLICATES' $(if ($PeerAllowDuplicates) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_EXPECT_NO_RECEIVE' $(if ($expectNoReceive) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_EMPTY_POLL_DELAY_US' ([string]$PeerEmptyPollDelayUs)
            $peerStdout = Join-Path $EvidenceDirectory 'peer.stdout.jsonl'
            $peerStderr = Join-Path $EvidenceDirectory 'peer.stderr.log'
            $peerStart = [Diagnostics.ProcessStartInfo]::new()
            $peerStart.FileName = $HarnessPath
            $peerStart.Arguments = 'can-hardware-receive'
            $peerStart.UseShellExecute = $false
            $peerStart.CreateNoWindow = $true
            $peerStart.RedirectStandardOutput = $true
            $peerStart.RedirectStandardError = $true
            $peerProcess = [Diagnostics.Process]::new()
            $peerProcess.StartInfo = $peerStart
            if (-not $peerProcess.Start()) {
                throw 'The receive peer process could not be started.'
            }
            Start-Sleep -Milliseconds 1000
            $peerProcess.Refresh()
            if ($peerProcess.HasExited) {
                $peerProcess.WaitForExit()
                [IO.File]::WriteAllText($peerStdout, $peerProcess.StandardOutput.ReadToEnd())
                [IO.File]::WriteAllText($peerStderr, $peerProcess.StandardError.ReadToEnd())
                throw "The receive peer exited before the transmitter started (exit $($peerProcess.ExitCode))."
            }

            Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $EndpointId
            Set-ProcessEnvironment 'UCAL_HARNESS_LISTEN_ONLY' $(if ($ListenOnly) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_NO_ACK' $(if ($NoAck) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $(switch ($Termination) {
                'On' { '1' }
                'Off' { '0' }
                default { $null }
            })
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_RECEIVED' $(if ($requireSelfReceived) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_SELF_RECEIVED' $(if ($requireSelfReceived) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_REQUIRE_TX_COMPLETION' $(if ($requireTxCompletion) { '1' } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_SUBMIT_ONLY' `
                $(if ($EvidenceMode -eq 'Receive' -or
                      ($EvidenceMode -eq 'RejectReceive' -and -not $requireTxCompletion)) {
                    '1'
                } else { '0' })
            Set-ProcessEnvironment 'UCAL_HARNESS_ALLOW_DUPLICATES' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_EXPECT_NO_RECEIVE' '0'
            Set-ProcessEnvironment 'UCAL_HARNESS_EMPTY_POLL_DELAY_US' ([string]$EmptyPollDelayUs)
            $smokeRun = Invoke-CapturedProcess $HarnessPath @('can-hardware-smoke') `
                (Join-Path $EvidenceDirectory 'smoke.stdout.jsonl') `
                (Join-Path $EvidenceDirectory 'smoke.stderr.log')
            $peerWaitMs = [int]$TimeoutMs + 5000
            if (-not $peerProcess.WaitForExit($peerWaitMs)) {
                throw 'The receive peer did not finish within its bounded timeout.'
            }
            $peerProcess.WaitForExit()
            $peerProcess.Refresh()
            $peerStdoutText = $peerProcess.StandardOutput.ReadToEnd()
            $peerStderrText = $peerProcess.StandardError.ReadToEnd()
            [IO.File]::WriteAllText($peerStdout, $peerStdoutText)
            [IO.File]::WriteAllText($peerStderr, $peerStderrText)
            $peerRun = [ordered]@{
                exitCode = $peerProcess.ExitCode
                stdout = $peerStdoutText
                stderr = $peerStderrText
            }
            $result.harness = [ordered]@{
                transmitter = [ordered]@{
                    exitCode = $smokeRun.exitCode
                    events = @(ConvertFrom-JsonLines $smokeRun.stdout)
                    stderr = $smokeRun.stderr
                }
                receiver = [ordered]@{
                    exitCode = $peerRun.exitCode
                    events = @(ConvertFrom-JsonLines $peerRun.stdout)
                    stderr = $peerRun.stderr
                }
            }
            if ($smokeRun.exitCode -ne 0 -or $peerRun.exitCode -ne 0) {
                throw "CAN peer smoke assertions failed: transmitter=$($smokeRun.exitCode), receiver=$($peerRun.exitCode)."
            }
        } else {
            $harnessCommand = if ($ConfigurationOnly) { 'can-hardware-config' } else { 'can-hardware-smoke' }
            $smokeRun = Invoke-CapturedProcess $HarnessPath @($harnessCommand) `
                (Join-Path $EvidenceDirectory 'smoke.stdout.jsonl') `
                (Join-Path $EvidenceDirectory 'smoke.stderr.log')
            $result.harness = [ordered]@{
                exitCode = $smokeRun.exitCode
                events = @(ConvertFrom-JsonLines $smokeRun.stdout)
                stderr = $smokeRun.stderr
            }
            if ($smokeRun.exitCode -ne 0) {
                throw "CAN hardware smoke assertions failed with exit code $($smokeRun.exitCode)."
            }
        }
        $result.timingVerification.passed = $true
        $result.status = 'passed'
        $result.stage = 'complete'
        $exitCode = 0
    }
} catch {
    $result.error = $_.Exception.Message
} finally {
    if ($peerProcess) {
        $peerProcess.Refresh()
        if (-not $peerProcess.HasExited) {
            Stop-Process -Id $peerProcess.Id -Force -ErrorAction SilentlyContinue
            $peerProcess.WaitForExit()
        }
    }
    if ($brokerProcess) {
        $brokerProcess.Refresh()
        if (-not $brokerProcess.HasExited) {
            $result.cleanup.shutdownRequested = $true
            $shutdownRun = Invoke-CapturedProcess $HarnessPath @('shutdown') `
                (Join-Path $EvidenceDirectory 'shutdown.stdout.log') `
                (Join-Path $EvidenceDirectory 'shutdown.stderr.log')
            $result.cleanup.shutdownExitCode = $shutdownRun.exitCode
            $null = $brokerProcess.WaitForExit(5000)
            $brokerProcess.Refresh()
        }
        if (-not $brokerProcess.HasExited) {
            Stop-Process -Id $brokerProcess.Id -Force -ErrorAction SilentlyContinue
            $brokerProcess.WaitForExit()
            $result.cleanup.forced = $true
            $exitCode = 1
            if (-not $result.error) {
                $result.error = 'Owned Broker required forced cleanup.'
                $result.status = 'error'
                $result.stage = 'cleanup'
            }
        }
        $result.cleanup.brokerExited = $brokerProcess.HasExited
        if ($result.cleanup.shutdownExitCode -ne 0) {
            $exitCode = 1
            if (-not $result.error) {
                $result.error = 'Owned Broker rejected or failed bounded shutdown.'
                $result.status = 'error'
                $result.stage = 'cleanup'
            }
        }
    }
    foreach ($name in $environmentNames) {
        Set-ProcessEnvironment $name $savedEnvironment[$name]
    }
    $result.completedUtc = [DateTime]::UtcNow.ToString('o')
}

$manifestPath = Join-Path $EvidenceDirectory 'result.json'
$result['output'] = [ordered]@{ directory = $EvidenceDirectory; manifest = $manifestPath }
[IO.File]::WriteAllText(
    $manifestPath,
    ($result | ConvertTo-Json -Depth 10),
    [Text.UTF8Encoding]::new($false))
$result | ConvertTo-Json -Depth 10 -Compress
exit $exitCode
