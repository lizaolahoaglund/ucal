[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Execute')]
    [string]$Mode = 'Plan',

    [Parameter(Mandatory = $true)]
    [string]$EndpointA,

    [Parameter(Mandatory = $true)]
    [string]$EndpointB,

    [Parameter(Mandatory = $true)]
    [string]$PnpInstanceIdA,

    [Parameter(Mandatory = $true)]
    [string]$PnpInstanceIdB,

    [Parameter(Mandatory = $true)]
    [string]$RuntimePath,

    [ValidateRange(1, 60)]
    [int]$PollIntervalSeconds = 1,

    [ValidateRange(5, 180)]
    [int]$TransitionTimeoutSeconds = 30,

    [ValidateRange(1, 255)]
    [int]$ExpectedOfflineOpenExitCode = 198,

    [string]$BrokerPath,
    [string]$HarnessPath,
    [string]$EvidenceDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
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

function Set-ProcessEnvironment([string]$Name, [AllowNull()][string]$Value) {
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}

function Get-EndpointCatalog {
    $lines = @(& $HarnessPath backend-endpoints)
    if ($LASTEXITCODE -ne 0) { throw 'Endpoint discovery failed.' }
    return @($lines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_ | ConvertFrom-Json })
}

function Set-PnpState([string]$InstanceId, [ValidateSet('Enabled', 'Disabled')][string]$State) {
    $verb = if ($State -eq 'Enabled') { '/enable-device' } else { '/disable-device' }
    $expectedProblemCode = if ($State -eq 'Enabled') { [uint32]0 } else { [uint32]22 }
    try {
        $currentProblemCode = [uint32](Get-PnpDeviceProperty -InstanceId $InstanceId `
            -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data
        if ($currentProblemCode -eq $expectedProblemCode) { return }
    } catch {}
    $output = @(& pnputil.exe $verb $InstanceId)
    if ($LASTEXITCODE -ne 0) {
        throw "PnP $State failed for the exact requested instance (exit $LASTEXITCODE): $($output -join ' ')"
    }
    $observedProblemCode = $null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        try {
            $observedProblemCode = [uint32](Get-PnpDeviceProperty -InstanceId $InstanceId `
                -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data
            if ($observedProblemCode -eq $expectedProblemCode) { return }
        } catch {}
        Start-Sleep -Milliseconds 250
    }
    throw "PnP command returned success but $State was not observed for the exact instance; problem code was $observedProblemCode. Output: $($output -join ' ')"
}

function Wait-EndpointState(
    [string]$Target,
    [uint32]$ExpectedTargetState,
    [string]$Peer,
    [string[]]$BaselineIds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TransitionTimeoutSeconds)
    do {
        $catalog = @(Get-EndpointCatalog)
        $pcan = @($catalog | Where-Object { [string]$_.backendFamily -eq 'pcan-basic' })
        $targetRows = @($pcan | Where-Object { [string]$_.endpoint -eq $Target })
        $peerRows = @($pcan | Where-Object { [string]$_.endpoint -eq $Peer })
        $currentIds = @($pcan | ForEach-Object { [string]$_.endpoint } | Sort-Object -Unique)
        $idsStable = @(Compare-Object -ReferenceObject $BaselineIds -DifferenceObject $currentIds).Count -eq 0
        if ($idsStable -and $targetRows.Count -eq 1 -and $peerRows.Count -eq 1 -and
            [uint32]$targetRows[0].state -eq $ExpectedTargetState -and
            [uint32]$peerRows[0].state -eq 0) {
            return @($catalog)
        }
        Start-Sleep -Seconds $PollIntervalSeconds
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Endpoint $Target did not reach state $ExpectedTargetState with stable PCAN identities while peer $Peer stayed online."
}

$uuidPattern = '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$'
if ($EndpointA -notmatch $uuidPattern -or $EndpointB -notmatch $uuidPattern -or
    $EndpointA -eq $EndpointB) {
    throw 'EndpointA and EndpointB must be two distinct exact endpoint UUIDs.'
}
if ([string]::IsNullOrWhiteSpace($PnpInstanceIdA) -or
    [string]::IsNullOrWhiteSpace($PnpInstanceIdB) -or
    $PnpInstanceIdA -eq $PnpInstanceIdB) {
    throw 'PnpInstanceIdA and PnpInstanceIdB must be two distinct exact instance IDs.'
}

$plan = [ordered]@{
    schemaVersion = 1
    mode = $Mode
    backend = 'PCAN'
    endpoints = @($EndpointA, $EndpointB)
    pnpTargets = @($PnpInstanceIdA, $PnpInstanceIdB)
    expectedOfflineState = 2
    expectedOfflineOpenExitCode = $ExpectedOfflineOpenExitCode
    invariant = 'same Broker; exact stable endpoint IDs; offline tombstone; peer online; offline open rejected; exact restoration'
}
if ($Mode -eq 'Plan') {
    [ordered]@{ status = 'planned'; plan = $plan } | ConvertTo-Json -Depth 6 -Compress
    exit 0
}

$BrokerPath = Resolve-RequiredFile $BrokerPath 'Broker'
$HarnessPath = Resolve-RequiredFile $HarnessPath 'Harness'
$RuntimePath = Resolve-RequiredFile $RuntimePath 'PCAN-Basic runtime'
if (Get-Process -Name 'UcalBroker' -ErrorAction SilentlyContinue) {
    throw 'A UCAL Broker is already running. This runner will not stop or reuse it.'
}
foreach ($instanceId in @($PnpInstanceIdA, $PnpInstanceIdB)) {
    $device = Get-PnpDevice -InstanceId $instanceId -ErrorAction Stop
    if ([string]$device.Status -ne 'OK') {
        throw "PnP preflight requires an online device: $instanceId ($($device.Status))"
    }
}
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $EvidenceDirectory = Join-Path $repoRoot "out\hardware-hotplug\$stamp-pcan"
}
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null

$savedEnvironment = @{}
foreach ($name in @('UCAL_BACKEND', 'UCAL_PCAN_BASIC_RUNTIME',
        'UCAL_HARNESS_ENDPOINT_ID', 'UCAL_HARNESS_CAN_FD',
        'UCAL_HARNESS_NBITRATE', 'UCAL_HARNESS_DBITRATE',
        'UCAL_HARNESS_TERMINATION')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$result = [ordered]@{
    schemaVersion = 1
    status = 'error'
    stage = 'preflight'
    startedUtc = [DateTime]::UtcNow.ToString('o')
    completedUtc = $null
    plan = $plan
    baseline = @()
    cycles = @()
    cleanup = [ordered]@{ bothDevicesEnabled = $false; brokerExited = $false; forced = $false }
    error = $null
    output = [ordered]@{ directory = $EvidenceDirectory; manifest = '' }
}
$broker = $null
$exitCode = 1
try {
    Set-ProcessEnvironment 'UCAL_BACKEND' 'PCAN'
    Set-ProcessEnvironment 'UCAL_PCAN_BASIC_RUNTIME' $RuntimePath
    Set-ProcessEnvironment 'UCAL_HARNESS_CAN_FD' '0'
    Set-ProcessEnvironment 'UCAL_HARNESS_NBITRATE' '500000'
    Set-ProcessEnvironment 'UCAL_HARNESS_DBITRATE' '2000000'
    Set-ProcessEnvironment 'UCAL_HARNESS_TERMINATION' $null

    Set-PnpState $PnpInstanceIdA Enabled
    Set-PnpState $PnpInstanceIdB Enabled
    $result.stage = 'broker-start'
    $broker = Start-Process -FilePath $BrokerPath -ArgumentList '--allow-test-shutdown' `
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

    $baseline = @(Get-EndpointCatalog)
    $result.baseline = $baseline
    $baselinePcan = @($baseline | Where-Object { [string]$_.backendFamily -eq 'pcan-basic' })
    $baselineIds = @($baselinePcan | ForEach-Object { [string]$_.endpoint } | Sort-Object -Unique)
    foreach ($endpoint in @($EndpointA, $EndpointB)) {
        $row = @($baselinePcan | Where-Object { [string]$_.endpoint -eq $endpoint })
        if ($row.Count -ne 1 -or [uint32]$row[0].state -ne 0) {
            throw "Baseline PCAN endpoint is missing, duplicated, or offline: $endpoint"
        }
    }

    foreach ($cycle in @(
            [ordered]@{ endpoint = $EndpointA; peer = $EndpointB; pnp = $PnpInstanceIdA },
            [ordered]@{ endpoint = $EndpointB; peer = $EndpointA; pnp = $PnpInstanceIdB })) {
        $result.stage = "disable-$($cycle.endpoint)"
        Set-PnpState $cycle.pnp Disabled
        $offline = @(Wait-EndpointState $cycle.endpoint 2 $cycle.peer $baselineIds)
        Set-ProcessEnvironment 'UCAL_HARNESS_ENDPOINT_ID' $cycle.endpoint
        $openOutput = @(& $HarnessPath can-hardware-config 2>&1)
        $openExit = $LASTEXITCODE
        if ($openExit -ne $ExpectedOfflineOpenExitCode) {
            throw "Offline open exit was $openExit, expected $ExpectedOfflineOpenExitCode."
        }

        $result.stage = "enable-$($cycle.endpoint)"
        Set-PnpState $cycle.pnp Enabled
        $restored = @(Wait-EndpointState $cycle.endpoint 0 $cycle.peer $baselineIds)
        $result.cycles += [ordered]@{
            endpoint = $cycle.endpoint
            peer = $cycle.peer
            idsStable = $true
            offlineTombstone = $true
            peerStayedOnline = $true
            offlineOpenExitCode = $openExit
            offlineOpenOutput = $openOutput
            restored = $true
            restoredCatalogCount = $restored.Count
        }
    }
    $result.status = 'passed'
    $result.stage = 'complete'
    $exitCode = 0
}
catch {
    $result.error = $_.Exception.Message
}
finally {
    try {
        Set-PnpState $PnpInstanceIdA Enabled
        Set-PnpState $PnpInstanceIdB Enabled
        $result.cleanup.bothDevicesEnabled = $true
    } catch {}
    if ($broker -and -not $broker.HasExited) {
        & $HarnessPath shutdown *> $null
        if (-not $broker.WaitForExit(5000)) {
            $broker.Kill()
            $broker.WaitForExit()
            $result.cleanup.forced = $true
        }
    }
    if ($broker) { $result.cleanup.brokerExited = $broker.HasExited }
    foreach ($name in $savedEnvironment.Keys) {
        Set-ProcessEnvironment $name $savedEnvironment[$name]
    }
    $result.completedUtc = [DateTime]::UtcNow.ToString('o')
    $manifest = Join-Path $EvidenceDirectory 'result.json'
    $result.output.manifest = $manifest
    [IO.File]::WriteAllText($manifest, ($result | ConvertTo-Json -Depth 10),
        [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Depth 10
}
exit $exitCode
