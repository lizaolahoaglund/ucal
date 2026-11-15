[CmdletBinding()]
param(
    [string]$SwitchPath,
    [string]$OutputDirectory,
    [ValidateRange(5, 60)][int]$TimeoutSeconds = 20
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
        'out\switch-process-isolation\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$testBrokerInstance='ucal_process_test_'+[guid]::NewGuid().ToString('N')
$testSettings=Join-Path $OutputDirectory 'settings'
$emptyBackends=Join-Path $OutputDirectory 'empty-backends'
New-Item -ItemType Directory -Force -Path $testSettings,$emptyBackends | Out-Null
$testBrokerPaths=@('runtime/x64/UcalBroker.exe','runtime/x86/UcalBroker.exe') | ForEach-Object {
    [IO.Path]::GetFullPath((Join-Path (Split-Path $SwitchPath -Parent) $_))
}

if (-not ('UcalManagerProcessUser32' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UcalManagerProcessUser32 {
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
}
'@
}

function Quote-Argument([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Start-OwnedSwitch([string[]]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $SwitchPath
    $info.WorkingDirectory = Split-Path -Parent $SwitchPath
    $info.UseShellExecute = $false
    $info.EnvironmentVariables['UCAL_BROKER_INSTANCE']=$testBrokerInstance
    $info.EnvironmentVariables['UCAL_MANAGER_SETTINGS_DIRECTORY']=$testSettings
    $info.EnvironmentVariables['UCAL_BACKEND_DIRECTORY']=$emptyBackends
    $info.Arguments = (($Arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Could not start UCAL Manager.' }
    return $process
}

function Wait-OwnedProcess([Diagnostics.Process]$Process, [string]$Label) {
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Process.Kill()
        $Process.WaitForExit()
        throw "$Label exceeded its bounded timeout."
    }
    if ($Process.ExitCode -ne 0) { throw "$Label exited with code $($Process.ExitCode)." }
}

$owned = New-Object Collections.Generic.List[Diagnostics.Process]
$result = [ordered]@{
    schemaVersion = 1
    status = 'error'
    executable = [ordered]@{
        path = $SwitchPath
        sha256 = (Get-FileHash -LiteralPath $SwitchPath -Algorithm SHA256).Hash
    }
    simultaneousInstances = $false
    crashIsolated = $false
    restartAfterCrash = $false
    startMinimized = $false
    closeToTray = $false
    trayUnavailableFallback = 'covered-by-widget-test'
    artifacts = @()
    error = $null
}

try {
    $firstWorkspace = Join-Path $OutputDirectory 'first-workspace.json'
    $secondWorkspace = Join-Path $OutputDirectory 'second-workspace.json'
    $firstImage = Join-Path $OutputDirectory 'first-before-crash.png'
    $secondImage = Join-Path $OutputDirectory 'second-survivor.png'
    $restartImage = Join-Path $OutputDirectory 'first-restarted.png'

    $first = Start-OwnedSwitch @('--presentation', 'production', '--language', 'en',
        '--live', '--workspace', $firstWorkspace, '--page', 'settings',
        '--screenshot', $firstImage)
    $owned.Add($first)
    $second = Start-OwnedSwitch @('--presentation', 'production', '--language', 'de',
        '--live', '--workspace', $secondWorkspace, '--page', 'hardware',
        '--screenshot', $secondImage)
    $owned.Add($second)
    Start-Sleep -Seconds 2
    $first.Refresh()
    $second.Refresh()
    if ($first.HasExited -or $second.HasExited -or $first.Id -eq $second.Id) {
        throw 'Two isolated UCAL Manager instances did not overlap as live processes.'
    }
    $result.simultaneousInstances = $true

    $first.Kill()
    $first.WaitForExit()
    Start-Sleep -Milliseconds 500
    $second.Refresh()
    if ($second.HasExited) { throw 'Crashing one owned instance terminated the other instance.' }
    $result.crashIsolated = $true
    Wait-OwnedProcess $second 'Surviving Switch instance'
    if (-not (Test-Path -LiteralPath $secondImage -PathType Leaf)) {
        throw 'The surviving instance did not write its screenshot.'
    }

    $restart = Start-OwnedSwitch @('--presentation', 'production', '--language', 'en',
        '--live', '--workspace', $firstWorkspace, '--page', 'settings',
        '--screenshot', $restartImage)
    $owned.Add($restart)
    Wait-OwnedProcess $restart 'Restarted Switch instance'
    if (-not (Test-Path -LiteralPath $restartImage -PathType Leaf)) {
        throw 'The crashed workspace did not restart into a rendered application.'
    }
    $result.restartAfterCrash = $true

    $minimized = Start-OwnedSwitch @('--presentation', 'production', '--language', 'en',
        '--start-minimized', '--workspace', (Join-Path $OutputDirectory 'minimized-workspace.json'))
    $owned.Add($minimized)
    Start-Sleep -Seconds 2
    $minimized.Refresh()
    if ($minimized.HasExited) {
        throw 'The start-minimized instance exited instead of remaining available in the tray.'
    }
    if ($minimized.MainWindowHandle -ne 0 -and
        [UcalManagerProcessUser32]::IsWindowVisible($minimized.MainWindowHandle)) {
        throw 'The --start-minimized instance exposed a visible main window.'
    }
    $result.startMinimized = $true
    $minimized.Kill()
    $minimized.WaitForExit()

    $tray = Start-OwnedSwitch @('--presentation', 'realistic', '--language', 'en',
        '--workspace', (Join-Path $OutputDirectory 'tray-workspace.json'))
    $owned.Add($tray)
    for ($attempt = 0; $attempt -lt 50 -and $tray.MainWindowHandle -eq 0; ++$attempt) {
        Start-Sleep -Milliseconds 100
        $tray.Refresh()
    }
    if ($tray.HasExited -or $tray.MainWindowHandle -eq 0) {
        throw 'The close-policy instance did not expose a main window.'
    }
    $window = $tray.MainWindowHandle
    if (-not [UcalManagerProcessUser32]::IsWindowVisible($window)) {
        throw 'The close-policy main window was not initially visible.'
    }
    if (-not $tray.CloseMainWindow()) { throw 'WM_CLOSE was not delivered to UCAL Manager.' }
    Start-Sleep -Seconds 1
    $tray.Refresh()
    if ($tray.HasExited -or [UcalManagerProcessUser32]::IsWindowVisible($window)) {
        throw 'Default close-to-tray policy did not keep the process alive with the window hidden.'
    }
    $result.closeToTray = $true
    $tray.Kill()
    $tray.WaitForExit()

    $result.artifacts = @(@($secondImage, $restartImage) | ForEach-Object {
        [ordered]@{
            path = $_
            bytes = (Get-Item -LiteralPath $_).Length
            sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
        }
    })
    $result.status = 'passed'
} catch {
    $result.error = $_.Exception.Message
} finally {
    $ownedManagerIds=@($owned | ForEach-Object { $_.Id })
    foreach ($process in $owned) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        } catch {}
    }
    # The crash cases deliberately kill their Managers. Clean up only Brokers
    # launched by those exact processes from this exact test package.
    foreach($broker in @(Get-CimInstance Win32_Process -Filter "Name='UcalBroker.exe'" |
        Where-Object {$_.ParentProcessId -in $ownedManagerIds -and $_.ExecutablePath -in $testBrokerPaths})){
        try{
            Stop-Process -Id $broker.ProcessId -ErrorAction Stop
            Wait-Process -Id $broker.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
        }catch{
            $result.status='error'
            $result.error='Test-owned Broker cleanup failed: '+$_.Exception.Message
        }
    }
}

$manifest = Join-Path $OutputDirectory 'manifest.json'
[IO.File]::WriteAllText($manifest, ($result | ConvertTo-Json -Depth 7),
                        (New-Object Text.UTF8Encoding($false)))
$result['manifest'] = $manifest
$result | ConvertTo-Json -Depth 7 -Compress
if ($result.status -ne 'passed') { exit 1 }
