[CmdletBinding()]
param(
    [string]$SwitchPath,

    [string]$OutputDirectory,

    [ValidateSet('production', 'realistic', 'explanatory')]
    [string]$Presentation = 'realistic',

    [string[]]$Languages = @('en', 'zh-CN', 'de', 'ja'),

    [string[]]$DpiScales = @('1.0', '1.25', '1.5', '2.0'),

    [string[]]$Pages = @('applications', 'hardware', 'software', 'drivers', 'settings'),

    [string[]]$Dialogs = @(
        'application',
        'application-channels',
        'application-integration',
        'project',
        'add-application',
        'hardware-assignment'
    ),

    [ValidateRange(5, 120)]
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($SwitchPath)) {
    $packaged = Join-Path $repoRoot 'out\UcalManager-product-package\UcalManager.exe'
    $SwitchPath = if (Test-Path -LiteralPath $packaged -PathType Leaf) {
        $packaged
    } else {
        Join-Path $repoRoot 'out\UcalManager-build\bin\UcalManager.exe'
    }
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot "out\switch-visual-matrix\$stamp"
}

if (-not (Test-Path -LiteralPath $SwitchPath -PathType Leaf)) {
    throw "UCAL Manager executable was not found: $SwitchPath"
}
$SwitchPath = (Resolve-Path -LiteralPath $SwitchPath).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function ConvertTo-QuotedArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-Screenshot(
    [string]$Language,
    [string]$DpiScale,
    [string]$SurfaceKind,
    [string]$SurfaceName) {
    $safeScale = $DpiScale.Replace('.', '_')
    $fileName = "$Language-$safeScale-$SurfaceKind-$SurfaceName.png"
    $outputPath = Join-Path $OutputDirectory $fileName
    $workspacePath = Join-Path $OutputDirectory "$Language-$safeScale-workspace.json"
    $arguments = @(
        '--presentation', $Presentation,
        '--language', $Language,
        '--workspace', $workspacePath,
        '--screenshot', $outputPath
    )
    if ($SurfaceKind -eq 'page') {
        $arguments += @('--page', $SurfaceName)
    } else {
        $arguments += @('--dialog', $SurfaceName)
    }

    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $SwitchPath
    $startInfo.WorkingDirectory = Split-Path -Parent $SwitchPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.Arguments = (($arguments | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join ' ')
    $startInfo.EnvironmentVariables['QT_SCALE_FACTOR'] = $DpiScale
    $startInfo.EnvironmentVariables['QT_AUTO_SCREEN_SCALE_FACTOR'] = '0'
    $startInfo.EnvironmentVariables['QT_ENABLE_HIGHDPI_SCALING'] = '1'

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Could not start $SwitchPath" }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Screenshot timed out: $Language / $DpiScale / $SurfaceKind / $SurfaceName"
    }
    if ($process.ExitCode -ne 0) {
        throw "Screenshot failed with exit code $($process.ExitCode): $Language / $DpiScale / $SurfaceKind / $SurfaceName"
    }
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        throw "Screenshot was not created: $outputPath"
    }
    $file = Get-Item -LiteralPath $outputPath
    if ($file.Length -lt 4096) {
        throw "Screenshot is unexpectedly small ($($file.Length) bytes): $outputPath"
    }

    Add-Type -AssemblyName System.Drawing
    $image = [Drawing.Image]::FromFile($outputPath)
    try {
        if ($image.Width -lt 600 -or $image.Height -lt 300) {
            throw "Screenshot dimensions are unexpectedly small ($($image.Width)x$($image.Height)): $outputPath"
        }
        return [ordered]@{
            language = $Language
            dpiScale = $DpiScale
            surfaceKind = $SurfaceKind
            surface = $SurfaceName
            path = $outputPath
            width = $image.Width
            height = $image.Height
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
            exitCode = $process.ExitCode
        }
    } finally {
        $image.Dispose()
        $process.Dispose()
    }
}

$results = @()
foreach ($language in $Languages) {
    if ($language -notin @('en', 'zh-CN', 'de', 'ja')) {
        throw "Unsupported language key: $language"
    }
    foreach ($scale in $DpiScales) {
        $parsedScale = 0.0
        if (-not [double]::TryParse(
                $scale,
                [Globalization.NumberStyles]::AllowDecimalPoint,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsedScale) -or $parsedScale -lt 1.0 -or $parsedScale -gt 4.0) {
            throw "DpiScales entries must be invariant values from 1.0 through 4.0: $scale"
        }
        foreach ($page in $Pages) {
            if ($page -notin @('applications', 'hardware', 'software', 'drivers', 'settings')) {
                throw "Unsupported page: $page"
            }
            $results += Invoke-Screenshot $language $scale 'page' $page
        }
        foreach ($dialog in $Dialogs) {
            if ($dialog -notin @('application', 'application-channels', 'application-integration',
                                 'project', 'add-application', 'hardware-assignment')) {
                throw "Unsupported dialog: $dialog"
            }
            $results += Invoke-Screenshot $language $scale 'dialog' $dialog
        }
    }
}

$expectedCount = $Languages.Count * $DpiScales.Count * ($Pages.Count + $Dialogs.Count)
if ($results.Count -ne $expectedCount) {
    throw "Visual matrix is incomplete: expected $expectedCount results, got $($results.Count)."
}

$manifest = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    executable = [ordered]@{
        path = $SwitchPath
        sha256 = (Get-FileHash -LiteralPath $SwitchPath -Algorithm SHA256).Hash
    }
    presentation = $Presentation
    expectedCount = $expectedCount
    passedCount = $results.Count
    results = $results
}
$manifestPath = Join-Path $OutputDirectory 'manifest.json'
[IO.File]::WriteAllText(
    $manifestPath,
    ($manifest | ConvertTo-Json -Depth 8),
    (New-Object Text.UTF8Encoding($false)))

[ordered]@{
    status = 'passed'
    count = $results.Count
    outputDirectory = $OutputDirectory
    manifest = $manifestPath
} | ConvertTo-Json -Depth 4 -Compress
