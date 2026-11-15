[CmdletBinding()]
param(
    [ValidateRange(1, 20)][int]$RestartIterations = 2,
    [string]$QmakePath = $env:UCAL_QMAKE,
    [string]$QmakeX86Path = $env:UCAL_QMAKE_X86,
    [string]$VcVarsAllPath = $env:UCAL_VCVARSALL,
    # Retained for callers of the older x64-only entry point.
    [string]$VcVars64Path = $env:UCAL_VCVARS64,
    [string]$PythonPath = 'python',
    [string]$OutputDirectory,
    [switch]$SkipCore,
    [switch]$SkipQt,
    [switch]$SkipPcanViewOffline,
    [switch]$RequirePrivateSupport
)
$ErrorActionPreference = 'Stop'
if ($SkipCore -and $SkipQt -and $SkipPcanViewOffline) { throw 'No regression suite was selected.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$powerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
Import-Module (Join-Path $PSScriptRoot '../tools/UcalToolchain.psm1') -Force
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo ('out/ci/regression-' + [Guid]::NewGuid().ToString('N')) }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) {
    throw 'OutputDirectory must be new or empty.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$summary = [ordered]@{ schema = 'org.ucal.deterministic-ci/2'; status = 'failed';
    core = 'not-requested'; qt = @(); pcanViewOffline = 'not-requested'; hardwareAccess = $false }
function Run-Stage([string]$Name, [string]$Script, [string[]]$Arguments) {
    Write-Host "Run $Name"
    $null = Invoke-UcalLoggedProcess $powerShell (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Script) + $Arguments) `
        $repo (Join-Path $output "$Name.log") -TimeoutSeconds 3600
}
try {
    if (-not $SkipCore) {
        $coreArguments = @('-RestartIterations', [string]$RestartIterations)
        if ($RequirePrivateSupport) { $coreArguments += '-RequirePrivateSupport' }
        Run-Stage 'core' (Join-Path $repo 'test/Run-Tests.ps1') $coreArguments
        $completeSources = (Test-Path -LiteralPath (Join-Path $repo 'frontends/vxlapi/src/XlRuntime.cpp')) -and
            (Test-Path -LiteralPath (Join-Path $repo 'frontends/pcanview/src/bridge/BridgeAdapter.cpp'))
        $summary.core = if ($completeSources) {
            'passed: complete source fixtures; Debug/Release x86/x64 builds'
        } else { 'passed: public core/backend/API fixtures; Debug/Release x86/x64 builds; private compatibility qualification not requested' }
    }
    $privateOffline = Test-Path -LiteralPath (Join-Path $repo 'frontends/pcanview/tools/pcanview_profile.py')
    if ($RequirePrivateSupport -and -not $SkipPcanViewOffline -and -not $privateOffline) {
        throw 'Private PCAN-View qualification sources are required for the offline suite.'
    }
    if (-not $SkipPcanViewOffline -and $privateOffline) {
        Run-Stage 'pcanview-offline' (Join-Path $repo 'frontends/pcanview/test/Test-PcanViewBridgeOffline.ps1') `
            @('-PythonPath', $PythonPath, '-Iterations', '2', '-Parallelism', '2', '-OutputDirectory', (Join-Path $output 'pcanview-offline'))
        $summary.pcanViewOffline = 'passed'
    }
    if (-not $SkipQt) {
        if (-not $VcVarsAllPath -and $VcVars64Path) {
            $VcVarsAllPath = Join-Path (Split-Path $VcVars64Path -Parent) 'vcvarsall.bat'
        }
        foreach ($architecture in @('x64', 'x86')) {
            if ($architecture -eq 'x86' -and -not $QmakeX86Path) { continue }
            $kit = if ($architecture -eq 'x64') { $QmakePath } else { $QmakeX86Path }
            $arguments = @('-Architecture', $architecture, '-OutputDirectory', (Join-Path $output "qt-$architecture"))
            if ($kit) { $arguments += @('-QmakePath', $kit) }
            if ($VcVarsAllPath) { $arguments += @('-VcVarsAllPath', $VcVarsAllPath) }
            Run-Stage "qt-$architecture" (Join-Path $PSScriptRoot '../manager/test/Test-UcalManager.ps1') $arguments
            $summary.qt += Get-Content -LiteralPath (Join-Path $output "qt-$architecture/summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        }
    }
    $summary.status = 'passed'
    $summary | ConvertTo-Json -Depth 10
} catch {
    $summary.error = $_.Exception.Message
    throw
} finally {
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'), ($summary | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
}
