[CmdletBinding()]
param(
    [string]$BrokerPath = 'out\x64\Release\UcalBroker.exe',
    [string]$HarnessPath = 'out\x64\Release\UcalHarness.exe',
    [ValidateRange(2, 32)]
    [int]$ClientCount = 12,
    [ValidateRange(1, 1000)]
    [int]$RequestsPerClient = 100
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repoRoot 'tools/UcalBrokerPipe.ps1')
function Resolve-UcalInput([string]$Path) {
    $candidate = if ([IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $repoRoot $Path }
    return (Resolve-Path -LiteralPath $candidate).Path
}
$broker = Resolve-UcalInput $BrokerPath
$harness = Resolve-UcalInput $HarnessPath
if (-not $env:UCAL_BROKER_INSTANCE -and (Get-Process -Name 'UcalBroker' -ErrorAction SilentlyContinue)) {
    throw 'Broker concurrency acceptance requires no pre-existing UcalBroker process.'
}

$logRoot = Join-Path $repoRoot 'out\test-logs'
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$stdout = Join-Path $logRoot 'broker-concurrency.stdout.log'
$stderr = Join-Path $logRoot 'broker-concurrency.stderr.log'
$brokerProcess = Start-Process -FilePath $broker -ArgumentList '--allow-test-shutdown' `
    -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$jobs = @()
$stall = $null

function Assert-UcalPing([int]$MaximumMilliseconds = 1500) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $harness ping *> $null
    $timer.Stop()
    if ($LASTEXITCODE -ne 0) { throw 'Broker ping failed.' }
    if ($timer.ElapsedMilliseconds -gt $MaximumMilliseconds) {
        throw "Broker ping exceeded $MaximumMilliseconds ms: $($timer.ElapsedMilliseconds) ms"
    }
    return $timer.ElapsedMilliseconds
}

function Invoke-MalformedHeader([uint16]$Minor, [uint16]$Reserved,
                                [uint32]$PayloadBytes) {
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipeName = Get-UcalBrokerPipeName
    $client = [IO.Pipes.NamedPipeClientStream]::new(
        '.', $pipeName, [IO.Pipes.PipeDirection]::InOut,
        [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $client.Connect(2000)
        $writer = [IO.BinaryWriter]::new($client, [Text.Encoding]::UTF8, $true)
        $writer.Write([uint32]0x49414355)
        $writer.Write([uint16]1)
        $writer.Write($Minor)
        $writer.Write([uint16]1)
        $writer.Write($Reserved)
        $writer.Write([uint64]42)
        $writer.Write($PayloadBytes)
        $writer.Write([int32]0)
        $writer.Flush()
        $reader = [IO.BinaryReader]::new($client, [Text.Encoding]::UTF8, $true)
        try {
            if ($reader.ReadUInt32() -ne 0x49414355) { throw 'Malformed request response magic changed.' }
            $null = $reader.ReadUInt16()
            $null = $reader.ReadUInt16()
            $null = $reader.ReadUInt16()
            $null = $reader.ReadUInt16()
            $null = $reader.ReadUInt64()
            $responseBytes = $reader.ReadUInt32()
            $status = $reader.ReadInt32()
            if ($responseBytes -ne 0 -or $status -ne 7) {
                throw "Malformed header was not rejected as transport_error: status=$status bytes=$responseBytes"
            }
        }
        catch [IO.EndOfStreamException] {
            # A fail-closed disconnect without a response body is also acceptable.
        }
    }
    finally {
        if ($reader) { try { $reader.Dispose() } catch {} }
        if ($writer) { try { $writer.Dispose() } catch {} }
        try { $client.Dispose() } catch {}
    }
}

function Invoke-RawRequest([uint16]$Operation, [byte[]]$Payload,
                           [int32]$ExpectedStatus, [uint16]$Minor = 7) {
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipeName = Get-UcalBrokerPipeName
    $client = [IO.Pipes.NamedPipeClientStream]::new(
        '.', $pipeName, [IO.Pipes.PipeDirection]::InOut,
        [IO.Pipes.PipeOptions]::Asynchronous)
    $writer = $null
    $reader = $null
    try {
        $client.Connect(2000)
        $writer = [IO.BinaryWriter]::new($client, [Text.Encoding]::UTF8, $true)
        $writer.Write([uint32]0x49414355)
        $writer.Write([uint16]1)
        $writer.Write($Minor)
        $writer.Write($Operation)
        $writer.Write([uint16]0)
        $writer.Write([uint64]43)
        $writer.Write([uint32]$Payload.Length)
        $writer.Write([int32]0)
        $writer.Write($Payload)
        $writer.Flush()
        $reader = [IO.BinaryReader]::new($client, [Text.Encoding]::UTF8, $true)
        if ($reader.ReadUInt32() -ne 0x49414355) { throw 'Raw request response magic changed.' }
        $responseMajor = $reader.ReadUInt16()
        $responseMinor = $reader.ReadUInt16()
        $null = $reader.ReadUInt16()
        $null = $reader.ReadUInt16()
        $null = $reader.ReadUInt64()
        $responseBytes = $reader.ReadUInt32()
        $status = $reader.ReadInt32()
        if ($responseMajor -ne 1 -or $responseMinor -ne $Minor -or
            $responseBytes -ne 0 -or $status -ne $ExpectedStatus) {
            throw "Raw request rejection changed: operation=$Operation status=$status bytes=$responseBytes"
        }
    }
    finally {
        if ($reader) { $reader.Dispose() }
        if ($writer) { $writer.Dispose() }
        $client.Dispose()
    }
}

try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        & $harness ping *> $null
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw 'Broker did not become ready.' }

    Invoke-RawRequest -Operation 1 -Payload ([byte[]]::new(0)) `
        -ExpectedStatus 0 -Minor 5

    for ($index = 0; $index -lt $ClientCount; ++$index) {
        $jobs += Start-Job -ScriptBlock {
            param($Probe, $Count)
            for ($request = 0; $request -lt $Count; ++$request) {
                & $Probe ping *> $null
                if ($LASTEXITCODE -ne 0) { throw "Concurrent ping failed at $request" }
            }
        } -ArgumentList $harness, $RequestsPerClient
    }
    $jobs | Wait-Job | Out-Null
    $failed = @($jobs | Where-Object State -ne 'Completed')
    if ($failed.Count -ne 0) {
        $jobs | Receive-Job
        throw "$($failed.Count) concurrent ping workers failed."
    }
    $jobs | Receive-Job

    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipeName = Get-UcalBrokerPipeName
    $stall = Start-Job -ScriptBlock {
        param($Name)
        $client = [IO.Pipes.NamedPipeClientStream]::new(
            '.', $Name, [IO.Pipes.PipeDirection]::InOut,
            [IO.Pipes.PipeOptions]::Asynchronous)
        $client.Connect(2000)
        Start-Sleep -Seconds 7
        $client.Dispose()
    } -ArgumentList $pipeName
    Start-Sleep -Milliseconds 500
    $parallelPingMs = Assert-UcalPing
    $stall | Wait-Job -Timeout 10 | Out-Null
    if ($stall.State -ne 'Completed') { throw 'Stalled client did not remain bounded.' }

    Invoke-MalformedHeader -Minor 65535 -Reserved 0 -PayloadBytes 0
    Invoke-MalformedHeader -Minor 7 -Reserved 1 -PayloadBytes 0
    Invoke-MalformedHeader -Minor 7 -Reserved 0 -PayloadBytes 65537
    $openCanReserved = [byte[]]::new(60)
    $openCanReserved[57] = 1
    Invoke-RawRequest -Operation 5 -Payload $openCanReserved -ExpectedStatus 1
    $openCanFlags = [byte[]]::new(60)
    $openCanFlags[56] = 0x80
    Invoke-RawRequest -Operation 5 -Payload $openCanFlags -ExpectedStatus 1
    $advanced = [byte[]]::new(120)
    [BitConverter]::GetBytes([uint32]500000).CopyTo($advanced,16)
    [BitConverter]::GetBytes([uint32]2000000).CopyTo($advanced,20)
    [BitConverter]::GetBytes([uint32]1).CopyTo($advanced,60)
    [BitConverter]::GetBytes([uint32]80000000).CopyTo($advanced,64)
    [BitConverter]::GetBytes([uint32]1).CopyTo($advanced,68)
    [BitConverter]::GetBytes([uint32]139).CopyTo($advanced,72)
    [BitConverter]::GetBytes([uint32]20).CopyTo($advanced,76)
    [BitConverter]::GetBytes([uint32]8).CopyTo($advanced,80)
    [BitConverter]::GetBytes([uint32]1).CopyTo($advanced,84)
    Invoke-RawRequest -Operation 25 -Payload $advanced -ExpectedStatus 2 -Minor 10
    Invoke-RawRequest -Operation 25 -Payload $advanced -ExpectedStatus 3 -Minor 11
    $advanced[112]=1
    Invoke-RawRequest -Operation 25 -Payload $advanced -ExpectedStatus 1 -Minor 11
    $advanced[112]=0; $advanced[80]=0
    Invoke-RawRequest -Operation 25 -Payload $advanced -ExpectedStatus 1 -Minor 11
    $advanced[80]=8; $advanced[60]=4
    Invoke-RawRequest -Operation 25 -Payload $advanced -ExpectedStatus 1 -Minor 11
    $writeCanReserved = [byte[]]::new(88)
    $writeCanReserved[18] = 1
    Invoke-RawRequest -Operation 6 -Payload $writeCanReserved -ExpectedStatus 1
    $openLinReserved = [byte[]]::new(28)
    $openLinReserved[20] = 3
    $openLinReserved[25] = 1
    Invoke-RawRequest -Operation 12 -Payload $openLinReserved -ExpectedStatus 1
    $writeLinReserved = [byte[]]::new(28)
    $writeLinReserved[19] = 1
    Invoke-RawRequest -Operation 13 -Payload $writeLinReserved -ExpectedStatus 1
    $configureLinReserved = [byte[]]::new(24)
    $configureLinReserved[10] = 1
    $configureLinReserved[13] = 1
    Invoke-RawRequest -Operation 16 -Payload $configureLinReserved -ExpectedStatus 1
    $batchShort = [byte[]]::new(15)
    Invoke-RawRequest -Operation 19 -Payload $batchShort -ExpectedStatus 1
    $batchZeroCount = [byte[]]::new(16)
    Invoke-RawRequest -Operation 19 -Payload $batchZeroCount -ExpectedStatus 1
    $batchReserved = [byte[]]::new(16)
    $batchReserved[8] = 1
    $batchReserved[12] = 1
    Invoke-RawRequest -Operation 19 -Payload $batchReserved -ExpectedStatus 1
    $batchMissingFrame = [byte[]]::new(16)
    $batchMissingFrame[8] = 1
    Invoke-RawRequest -Operation 19 -Payload $batchMissingFrame -ExpectedStatus 1
    $batchFrameReserved = [byte[]]::new(104)
    $batchFrameReserved[8] = 1
    $batchFrameReserved[26] = 1
    Invoke-RawRequest -Operation 19 -Payload $batchFrameReserved -ExpectedStatus 1
    $postMalformedPingMs = Assert-UcalPing

    & $harness shutdown *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Owned Broker shutdown failed.' }
    if (-not $brokerProcess.WaitForExit(5000)) { throw 'Broker shutdown timed out.' }

    [ordered]@{
        schema = 'org.ucal.broker-concurrency-acceptance/1'
        status = 'passed'
        clients = $ClientCount
        requestsPerClient = $RequestsPerClient
        parallelPingMilliseconds = $parallelPingMs
        postMalformedPingMilliseconds = $postMalformedPingMs
        clientIoDeadlineMilliseconds = 5000
        malformedCases = @(
            'legacy-minor-response', 'future-minor', 'nonzero-header-reserved', 'oversize-payload',
            'open-can-reserved', 'open-can-unknown-flags', 'advanced-older-minor', 'advanced-supported-envelope',
            'advanced-reserved', 'advanced-invalid-sjw', 'advanced-unknown-flags', 'write-can-reserved',
            'open-lin-reserved', 'write-lin-reserved', 'configure-lin-reserved',
            'batch-short', 'batch-zero-count', 'batch-header-reserved',
            'batch-missing-frame', 'batch-frame-reserved')
    } | ConvertTo-Json -Depth 5
}
finally {
    if ($jobs) { $jobs | Remove-Job -Force -ErrorAction SilentlyContinue }
    if ($stall) { $stall | Remove-Job -Force -ErrorAction SilentlyContinue }
    $brokerProcess.Refresh()
    if (-not $brokerProcess.HasExited) {
        Stop-Process -Id $brokerProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $brokerProcess.Dispose()
}
