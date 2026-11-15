Set-StrictMode -Version 2

function Find-UcalVisualStudio {
    $command = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    $vswhere = if ($command) { $command.Source } else {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio C++ Build Tools were not found. Install the Desktop development with C++ workload, or provide the tool paths explicitly.'
    }
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'No Visual Studio installation with x86/x64 C++ tools was found.' }
    return [string](@($installation)[0])
}

function Get-UcalMSBuildPath([string]$Path) {
    if ($Path) { return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path }
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return (Resolve-Path -LiteralPath (Join-Path (Find-UcalVisualStudio) 'MSBuild\Current\Bin\MSBuild.exe')).Path
}

function Import-UcalMsvcEnvironment([string]$Architecture, [string]$VcVarsAllPath) {
    if (-not $VcVarsAllPath) {
        $VcVarsAllPath = Join-Path (Find-UcalVisualStudio) 'VC\Auxiliary\Build\vcvarsall.bat'
    }
    $path = (Resolve-Path -LiteralPath $VcVarsAllPath -ErrorAction Stop).Path
    if ($path.Contains('"') -or $path.Contains('%')) { throw 'The compiler environment path contains unsupported command-shell characters.' }
    # Repeated architecture switches must not keep appending compiler paths.
    # Use the toolchain's own cleanup in the child shell before reinitializing.
    $command = 'call "{0}" {1} >nul && set' -f $path, $Architecture
    if ($env:VSCMD_VER) {
        $command = 'call "{0}" /clean_env >nul && {1}' -f $path, $command
    }
    $lines = @(& $env:COMSPEC /d /s /c $command)
    if ($LASTEXITCODE -ne 0) { throw "Cannot initialize the MSVC $Architecture environment." }
    foreach ($line in $lines) {
        $separator = ([string]$line).IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(([string]$line).Substring(0, $separator),
                ([string]$line).Substring($separator + 1), 'Process')
        }
    }
    return $path
}

# CommandLineToArgvW/MSVC argument quoting, including spaces, quotes and trailing
# backslashes. ProcessStartInfo.ArgumentList is unavailable in Windows PowerShell 5.1.
function ConvertTo-UcalProcessArgument([AllowEmptyString()][string]$Value) {
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Invoke-UcalLoggedProcess {
    param([string]$Program, [string[]]$Arguments, [string]$Directory,
          [string]$LogPath, [int]$TimeoutSeconds = 900, [switch]$AllowFailure)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Program
    $start.Arguments = (($Arguments | ForEach-Object { ConvertTo-UcalProcessArgument $_ }) -join ' ')
    $start.WorkingDirectory = $Directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "Cannot start $Program" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            $process.Kill()
            $process.WaitForExit()
        }
        $process.WaitForExit()
        $body = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($LogPath, $body, [Text.UTF8Encoding]::new($false))
        if ($timedOut) { throw "Timed out after $TimeoutSeconds seconds: $Program. See $LogPath" }
        $code = $process.ExitCode
        if ($code -ne 0 -and -not $AllowFailure) { throw "Exit code $code from $Program. See $LogPath" }
        return $code
    } finally { $process.Dispose() }
}

Export-ModuleMember -Function Find-UcalVisualStudio, Get-UcalMSBuildPath, Import-UcalMsvcEnvironment, ConvertTo-UcalProcessArgument, Invoke-UcalLoggedProcess
