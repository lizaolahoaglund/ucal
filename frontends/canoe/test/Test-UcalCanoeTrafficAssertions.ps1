[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [string]$RuntimeDirectory
)
$ErrorActionPreference = 'Stop'
if (-not $RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $PSScriptRoot '../../../out/x64/Release'
}
Import-Module (Join-Path $PSScriptRoot 'CanoeTrafficAcceptance.psm1') -Force
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$brokerPath = Join-Path $runtime 'UcalBroker.exe'
$harness = Join-Path $runtime 'UcalHarness.exe'
$fixture = Join-Path $runtime 'UcalBackendFixture.dll'
foreach ($path in @($brokerPath, $harness, $fixture)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test runtime $path" }
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Use a new evidence directory.' }
$directory = (New-Item -ItemType Directory -Path $EvidenceDirectory).FullName
$saved = @{}
$names = @('UCAL_BACKEND', 'UCAL_BACKEND_CATALOG', 'UCAL_BROKER_INSTANCE') +
    @(Get-ChildItem Env:UCAL_HARNESS_* | ForEach-Object Name)
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$broker = $null
try {
    Get-ChildItem Env:UCAL_HARNESS_* | Remove-Item
    $options = Join-Path $directory 'fixture.ini'
    "[fixture]`nchannels=1`ncan_fd=1`n" | Set-Content -LiteralPath $options -Encoding Unicode
    $catalog = Join-Path $directory 'catalog.ini'
    "[catalog]`nversion=1`ncount=1`n[plugin.0]`nid=fixture`nfamily=ucal`nmode=ucal`nlibrary=$fixture`nconfiguration=$options`nworker=$brokerPath`n" |
        Set-Content -LiteralPath $catalog -Encoding Unicode
    $env:UCAL_BACKEND = 'PLUGINS'
    $env:UCAL_BACKEND_CATALOG = $catalog
    $env:UCAL_BROKER_INSTANCE = 'canoe-assertions-' + [guid]::NewGuid().ToString('N')
    $broker = Start-Process -FilePath $brokerPath -ArgumentList '--allow-test-shutdown' -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $directory 'broker.log') `
        -RedirectStandardError (Join-Path $directory 'broker.stderr.log')
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        & $harness ping *> $null
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) { throw 'Virtual test Broker did not become ready.' }
    $env:UCAL_HARNESS_FRAME_COUNT = '16'
    $env:UCAL_HARNESS_CAN_FD = '1'
    $env:UCAL_HARNESS_PAYLOAD_LENGTH = '64'
    $env:UCAL_HARNESS_REQUIRE_RECEIVED = '1'
    $env:UCAL_HARNESS_REQUIRE_TX_COMPLETION = '0'
    $output = & $harness can-hardware-smoke
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $directory 'mock-traffic.jsonl') -Encoding utf8
    $summary = @($output | ConvertFrom-Json | Where-Object canHardwareSmoke)
    if ($summary.Count -ne 1) { throw 'Expected one virtual receiver summary.' }
    $parameters = @{ MeasurementRunning = $true; ExitCode = $code; Summary = $summary[0]; FrameCount = 16; PayloadLength = 64 }
    Assert-UcalCanoeTrafficResult @parameters
    $cases = @('count', 'order', 'content', 'duplicate', 'missing', 'measurement', 'exit')
    foreach ($case in $cases) {
        $copy = $summary[0] | ConvertTo-Json | ConvertFrom-Json
        $parameters.Summary = $copy
        $parameters.MeasurementRunning = $case -ne 'measurement'
        $parameters.ExitCode = if ($case -eq 'exit') { 1 } else { 0 }
        switch ($case) {
            'count' { $copy.receivedCount = 15 }
            'order' { $copy.outOfOrderCount = 1 }
            'content' { $copy.unexpectedCount = 1 }
            'duplicate' { $copy.duplicateCount = 1 }
            'missing' { $copy.PSObject.Properties.Remove('unexpectedCount') }
        }
        $rejected = $false
        try { Assert-UcalCanoeTrafficResult @parameters } catch { $rejected = $true }
        if (-not $rejected) { throw "The $case regression was accepted." }
    }
    $capture = Invoke-UcalCanoeDiagnosticCapture { throw 'Injected screenshot failure' }
    if ($capture.status -ne 'unavailable' -or $capture.error -ne 'Injected screenshot failure') {
        throw 'Screenshot failure was not retained as diagnostic evidence.'
    }
    Assert-UcalCanoeTrafficResult -MeasurementRunning $true -ExitCode $code -Summary $summary[0] -FrameCount 16 -PayloadLength 64
    $result = [ordered]@{ status = 'passed'; backend = 'original virtual fixture'; cases = 9; physicalTraffic = $false }
} finally {
    $cleanupError = ''
    if ($broker) {
        & $harness shutdown *> $null
        $shutdownCode = $LASTEXITCODE
        if (-not $broker.WaitForExit(5000)) {
            $broker.Kill()
            $broker.WaitForExit()
            $cleanupError = 'Virtual test Broker did not close normally.'
        }
        if ($shutdownCode -ne 0) { $cleanupError = 'Virtual test Broker shutdown failed.' }
        $broker.Dispose()
    }
    Get-ChildItem Env:UCAL_HARNESS_* | Remove-Item
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if ($cleanupError) { throw $cleanupError }
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding utf8
