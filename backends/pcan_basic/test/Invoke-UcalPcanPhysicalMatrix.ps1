[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EndpointA,

    [Parameter(Mandatory = $true)]
    [string]$EndpointB,

    [Parameter(Mandatory = $true)]
    [string]$RuntimePath,

    [ValidateSet('x64', 'Win32')]
    [string]$Architecture = 'x64',

    [ValidateRange(1, 100)]
    [int]$LifecycleCycles = 10,

    [string]$EvidenceDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$runner = (Resolve-Path (Join-Path $PSScriptRoot '../../../ucal/test/hardware/Invoke-UcalCanHardwareSmoke.ps1')).Path
$RuntimePath = (Resolve-Path -LiteralPath $RuntimePath).Path
if ($EndpointA -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$' -or
    $EndpointB -notmatch '^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$' -or
    $EndpointA -eq $EndpointB) {
    throw 'EndpointA and EndpointB must be distinct exact UUIDs from one current PCAN discovery.'
}
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $EvidenceDirectory = Join-Path $repoRoot "out\hardware-acceptance\$stamp-pcan-matrix"
}
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$powerShell = (Get-Process -Id $PID).Path
$records = [Collections.Generic.List[object]]::new()

function Invoke-MatrixCase([string]$Name, [hashtable]$Arguments) {
    $caseDirectory = Join-Path $EvidenceDirectory $Name
    $stdout = Join-Path $caseDirectory 'runner.stdout.jsonl'
    $stderr = Join-Path $caseDirectory 'runner.stderr.log'
    New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
    $argumentList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runner)
    foreach ($entry in $Arguments.GetEnumerator()) {
        if ($entry.Value -is [bool] -and -not $entry.Value) { continue }
        $argumentList += "-$($entry.Key)"
        if ($entry.Value -isnot [Management.Automation.SwitchParameter] -and
            $entry.Value -isnot [bool]) {
            $argumentList += [string]$entry.Value
        }
    }
    $argumentList += @('-EvidenceDirectory', $caseDirectory)
    $process = Start-Process -FilePath $powerShell -ArgumentList $argumentList `
        -PassThru -Wait -WindowStyle Hidden `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $lines = @([IO.File]::ReadAllLines($stdout) | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    })
    $result = $null
    $parseError = $null
    if ($lines.Count -gt 0) {
        try { $result = $lines[-1] | ConvertFrom-Json } catch {
            $parseError = $_.Exception.Message
        }
    }
    $records.Add([pscustomobject][ordered]@{
        name = $Name
        exitCode = $process.ExitCode
        status = if ($null -eq $result) { 'no-result' } else { [string]$result.status }
        stage = if ($null -eq $result) { $null } else { [string]$result.stage }
        manifest = if ($null -eq $result) { $null } else { [string]$result.output.manifest }
        error = if ($null -ne $parseError) {
            "Result JSON parse failed: $parseError"
        } elseif ($null -eq $result) {
            [IO.File]::ReadAllText($stderr)
        } else {
            [string]$result.error
        }
        catalog = if ($null -eq $result) { @() } else { @($result.endpointCatalog) }
    })
}

function New-BaseArguments {
    return @{
        Backend = 'PCAN'
        Mode = 'Execute'
        Architecture = $Architecture
        EndpointId = $EndpointA
        PeerEndpointId = $EndpointB
        EvidenceMode = 'Receive'
        Termination = 'Auto'
        RuntimePath = $RuntimePath
        TimeoutMs = 5000
        FrameCount = 20
    }
}

for ($cycle = 1; $cycle -le 3; ++$cycle) {
    Invoke-MatrixCase ("identity-{0:d2}" -f $cycle) @{
        Backend = 'PCAN'
        Mode = 'Discover'
        Architecture = $Architecture
        RuntimePath = $RuntimePath
    }
}

foreach ($bitrate in @(125000, 250000, 500000, 1000000)) {
    $arguments = New-BaseArguments
    $arguments.NominalBitrate = $bitrate
    Invoke-MatrixCase "classic-$bitrate" $arguments
}
foreach ($timing in @(
        @{ Nominal = 250000; Data = 1000000 },
        @{ Nominal = 250000; Data = 2000000 },
        @{ Nominal = 500000; Data = 1000000 },
        @{ Nominal = 500000; Data = 2000000 },
        @{ Nominal = 500000; Data = 4000000 },
        @{ Nominal = 1000000; Data = 2000000 },
        @{ Nominal = 1000000; Data = 4000000 }
    )) {
    $arguments = New-BaseArguments
    $arguments.CanFd = $true
    $arguments.NominalBitrate = $timing.Nominal
    $arguments.DataBitrate = $timing.Data
    $arguments.PayloadLength = 64
    Invoke-MatrixCase "fd-$($timing.Nominal)-$($timing.Data)" $arguments
}

foreach ($payload in @(0, 1, 2, 3, 4, 5, 6, 7, 8)) {
    $arguments = New-BaseArguments
    $arguments.PayloadLength = $payload
    if ($payload -eq 0) { $arguments.ZeroLengthPayload = $true }
    if ($payload -lt 4) { $arguments.FrameCount = 1 }
    Invoke-MatrixCase "classic-payload-$payload" $arguments
}
foreach ($payload in @(0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)) {
    $arguments = New-BaseArguments
    $arguments.CanFd = $true
    $arguments.PayloadLength = $payload
    if ($payload -eq 0) { $arguments.ZeroLengthPayload = $true }
    if ($payload -lt 4) { $arguments.FrameCount = 1 }
    Invoke-MatrixCase "fd-payload-$payload" $arguments
}

$arguments = New-BaseArguments
$arguments.ExtendedFrame = $true
Invoke-MatrixCase 'extended-frame' $arguments
$arguments = New-BaseArguments
$arguments.RemoteFrame = $true
$arguments.RemoteDlc = 8
$arguments.FrameCount = 1
Invoke-MatrixCase 'remote-frame' $arguments
$arguments = New-BaseArguments
$arguments.ExtendedFrame = $true
$arguments.RemoteFrame = $true
$arguments.RemoteDlc = 3
$arguments.FrameCount = 1
Invoke-MatrixCase 'extended-remote-frame' $arguments

$arguments = New-BaseArguments
$arguments.StandardFilter = $true
$arguments.StandardFilterCode = 0x5a5
$arguments.StandardFilterMask = 0x7ff
Invoke-MatrixCase 'standard-filter-match' $arguments
$arguments = New-BaseArguments
$arguments.EvidenceMode = 'RejectReceive'
$arguments.StandardFilter = $true
$arguments.StandardFilterCode = 0x123
$arguments.StandardFilterMask = 0x7ff
$arguments.TimeoutMs = 1000
Invoke-MatrixCase 'standard-filter-reject' $arguments
$arguments = New-BaseArguments
$arguments.ExtendedFrame = $true
$arguments.ExtendedFilter = $true
$arguments.ExtendedFilterCode = 0x18daf110
$arguments.ExtendedFilterMask = 0x1fffffff
Invoke-MatrixCase 'extended-filter-match' $arguments
$arguments = New-BaseArguments
$arguments.EvidenceMode = 'RejectReceive'
$arguments.ExtendedFrame = $true
$arguments.ExtendedFilter = $true
$arguments.ExtendedFilterCode = 0x18daf111
$arguments.ExtendedFilterMask = 0x1fffffff
$arguments.TimeoutMs = 1000
Invoke-MatrixCase 'extended-filter-reject' $arguments

$arguments = New-BaseArguments
$arguments.PeerListenOnly = $true
$arguments.PeerAllowDuplicates = $true
$arguments.FrameCount = 1
$arguments.TimeoutMs = 5000
Invoke-MatrixCase 'listen-only-electrical' $arguments

for ($cycle = 1; $cycle -le $LifecycleCycles; ++$cycle) {
    $arguments = @{
        Backend = 'PCAN'
        Mode = 'Execute'
        Architecture = $Architecture
        EndpointId = if (($cycle % 2) -eq 0) { $EndpointB } else { $EndpointA }
        EvidenceMode = 'Receive'
        Termination = 'Auto'
        RuntimePath = $RuntimePath
        ConfigurationOnly = $true
        NominalBitrate = if (($cycle % 3) -eq 0) { 250000 } else { 500000 }
    }
    if (($cycle % 2) -eq 0) {
        $arguments.CanFd = $true
        $arguments.DataBitrate = 2000000
    }
    Invoke-MatrixCase ("lifecycle-{0:d2}" -f $cycle) $arguments
}

$identityRecords = @($records | Where-Object { $_.name -like 'identity-*' })
$identitySignatures = @($identityRecords | ForEach-Object {
    (@($_.catalog | Sort-Object physicalIndex | ForEach-Object {
        '{0}|{1}|{2}|{3}|{4}' -f $_.endpoint, $_.adapter, $_.physicalIndex,
            $_.capabilities, $_.state
    }) -join ';')
} | Select-Object -Unique)
$failures = @($records | Where-Object { $_.exitCode -ne 0 -or $_.status -notin @('passed', 'discovered') })
$result = [ordered]@{
    schema = 'org.ucal.pcan-physical-matrix/1'
    status = if ($failures.Count -eq 0 -and $identitySignatures.Count -eq 1) { 'passed' } else { 'failed' }
    startedCaseCount = $records.Count
    passedCaseCount = @($records | Where-Object { $_.exitCode -eq 0 }).Count
    identityStable = $identitySignatures.Count -eq 1
    lifecycleCycles = $LifecycleCycles
    records = @($records)
    failures = $failures
    outputDirectory = $EvidenceDirectory
}
$manifest = Join-Path $EvidenceDirectory 'pcan-physical-matrix.json'
$result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifest -Encoding UTF8
$result | ConvertTo-Json -Depth 10 -Compress
if ($result.status -ne 'passed') { exit 1 }
