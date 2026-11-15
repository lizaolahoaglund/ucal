function Assert-UcalCanoeTrafficResult {
    param(
        [Parameter(Mandatory)][bool]$MeasurementRunning,
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)]$Summary,
        [Parameter(Mandatory)][uint32]$FrameCount,
        [Parameter(Mandatory)][uint32]$PayloadLength
    )
    if (-not $MeasurementRunning) { throw 'CANoe measurement did not enter Running state.' }
    $required = @('canHardwareSmoke', 'result', 'frameCount', 'payloadLength',
        'receivedCount', 'duplicateCount', 'unexpectedCount', 'outOfOrderCount',
        'timestamped', 'closed')
    foreach ($name in $required) {
        if ($null -eq $Summary.PSObject.Properties[$name]) {
            throw "The receiver summary is missing $name."
        }
    }
    if ($ExitCode -ne 0 -or $Summary.result -ne 0 -or $Summary.canHardwareSmoke -ne $true -or
        $Summary.frameCount -ne $FrameCount -or $Summary.payloadLength -ne $PayloadLength -or
        $Summary.receivedCount -ne $FrameCount -or $Summary.duplicateCount -ne 0 -or
        $Summary.unexpectedCount -ne 0 -or $Summary.outOfOrderCount -ne 0 -or
        $Summary.timestamped -ne $true -or $Summary.closed -ne $true) {
        throw 'The receiver failed count, content, ordering, timestamp or close assertions.'
    }
}

function Invoke-UcalCanoeDiagnosticCapture {
    param([Parameter(Mandatory)][scriptblock]$Capture)
    try {
        [pscustomobject]@{ status = 'captured'; evidence = (& $Capture); error = '' }
    } catch {
        [pscustomobject]@{ status = 'unavailable'; evidence = $null; error = $_.Exception.Message }
    }
}

function Invoke-CanoeMeasurementControl(
    [Diagnostics.Process]$Process,
    [ValidateSet('State', 'Start', 'Stop')][string]$Action) {
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    $helper = Join-Path $PSScriptRoot 'software/measurement.py'
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $python
    $info.Arguments = '-B -X utf8 "' + $helper + '" --pid ' + $Process.Id +
        ' --executable "' + $Process.Path + '" --action ' + $Action
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $child = New-Object Diagnostics.Process
    $child.StartInfo = $info
    try {
        if (-not $child.Start()) { throw 'Could not start the CANoe measurement control.' }
        $output = $child.StandardOutput.ReadToEndAsync()
        $errors = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit(15000)) {
            $child.Kill()
            $child.WaitForExit()
            throw 'The CANoe measurement control exceeded its bounded timeout.'
        }
        $state = $output.Result | ConvertFrom-Json
        if ($child.ExitCode -ne 0 -or $state.status -ne 'passed') {
            throw "CANoe measurement control failed: $($state.error) $($errors.Result)"
        }
        return $state
    } finally { $child.Dispose() }
}

Export-ModuleMember -Function Assert-UcalCanoeTrafficResult, Invoke-UcalCanoeDiagnosticCapture, Invoke-CanoeMeasurementControl
