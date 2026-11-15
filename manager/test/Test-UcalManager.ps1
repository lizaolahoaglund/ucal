[CmdletBinding()]
param(
    [string]$QmakePath = $env:UCAL_QMAKE,
    [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
    [string]$VcVarsAllPath = $env:UCAL_VCVARSALL,
    [string]$OutputDirectory,
    [ValidateRange(30, 1800)][int]$TestTimeoutSeconds = 180,
    [ValidateRange(60, 3600)][int]$BuildTimeoutSeconds = 900
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'The maintained Switch regression target is Windows with Qt 5.14.2 and MSVC C++ tools.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $PSScriptRoot '../../tools/UcalToolchain.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'UcalQtResults.psm1') -Force
if (-not $QmakePath) {
    $command = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if ($command) { $QmakePath = $command.Source }
}
if (-not $QmakePath) { throw 'Pass -QmakePath or set UCAL_QMAKE to the qmake.exe of your Qt 5.14.2 MSVC kit.' }
$qmake = (Resolve-Path -LiteralPath $QmakePath).Path
$qtBin = Split-Path $qmake -Parent
$version = (& $qmake -query QT_VERSION | Out-String).Trim()
$spec = (& $qmake -query QMAKE_XSPEC | Out-String).Trim()
if ($version -ne '5.14.2' -or $spec -ne 'win32-msvc') {
    throw "Use the maintained Qt 5.14.2 MSVC kit. Selected version/spec: $version / $spec."
}
$archData = (& $qmake -query QT_INSTALL_ARCHDATA | Out-String).Trim()
$qconfig = Get-Content -LiteralPath (Join-Path $archData 'mkspecs/qconfig.pri')
$qtArch = ((@($qconfig | Where-Object { $_ -match '^QT_ARCH\s*=' })[0]) -split '=', 2)[1].Trim()
$expectedQtArch = if ($Architecture -eq 'x64') { 'x86_64' } else { 'i386' }
if ($qtArch -ne $expectedQtArch) { throw "The $Architecture run requires a matching Qt kit; this kit is $qtArch." }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('out/regression/switch-' + $Architecture + '-' + [Guid]::NewGuid().ToString('N'))
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) {
    throw 'OutputDirectory must be new or empty. Each run builds from source and retains its own evidence.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$saved = @{}
Get-ChildItem Env: | ForEach-Object { $saved[$_.Name] = $_.Value }
$summary = [ordered]@{ schema = 'org.ucal.manager-regression/1'; status = 'failed'; architecture = $Architecture;
    platform = 'windows'; qtVersion = $version;
    hardwareAccess = $false; cases = @(); outputDirectory = $output }
function Build-QtProject([string]$Name, [string]$Project, [string[]]$QmakeArguments = @()) {
    Write-Host "Build $Name ($Architecture)"
    $directory = Join-Path $output $Name
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $null = Invoke-UcalLoggedProcess $qmake (@($Project, 'CONFIG+=release', 'CONFIG-=debug') + $QmakeArguments) $directory `
        (Join-Path $directory 'qmake.log') -TimeoutSeconds $BuildTimeoutSeconds
    $null = Invoke-UcalLoggedProcess $make @('/NOLOGO') $directory `
        (Join-Path $directory 'build.log') -TimeoutSeconds $BuildTimeoutSeconds
    return $directory
}
try {
    & (Join-Path $PSScriptRoot 'Test-UcalQtReport.ps1') -OutputDirectory (Join-Path $output 'report-contract') | Out-Null
    Get-ChildItem Env: | Where-Object { $_.Name -like 'UCAL_*' } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
    $summary.compilerEnvironment = Import-UcalMsvcEnvironment $Architecture $VcVarsAllPath
    $env:PATH = $qtBin + ';' + $env:PATH
    $env:QT_PLUGIN_PATH = Join-Path (Split-Path $qtBin -Parent) 'plugins'
    $env:QT_QPA_PLATFORM = $summary.platform
    $make = (Get-Command nmake.exe -ErrorAction Stop).Source
    $settings = Join-Path $output 'settings'
    $emptyBackends = Join-Path $output 'empty-backends'
    New-Item -ItemType Directory -Force -Path $settings, $emptyBackends | Out-Null
    $env:UCAL_MANAGER_SETTINGS_DIRECTORY = $settings
    $env:UCAL_BACKEND_DIRECTORY = $emptyBackends
    $env:UCAL_BROKER_INSTANCE = 'ucal_qt_regression_' + [Guid]::NewGuid().ToString('N')
    $env:UCAL_MANAGER_REVIEW_IMAGES = Join-Path $output 'screenshots'
    $fixture = Build-QtProject 'fixture' (Join-Path $repo 'ucal/test/plugins/frontend/mocks/Mock.pro')
    $env:UCAL_FRONTEND_FIXTURE_DLL = Join-Path $fixture 'bin/UcalFrontendFixture.dll'
    $legacyFixture = Build-QtProject 'fixture-legacy' (Join-Path $repo 'ucal/test/plugins/frontend/mocks/Mock.pro') @('CONFIG+=legacy_exports')
    $env:UCAL_FRONTEND_LEGACY_FIXTURE_DLL = Join-Path $legacyFixture 'bin/UcalFrontendLegacyFixture.dll'
    $frontendRoot = Join-Path $output 'frontends'
    $descriptors = @(Get-ChildItem -LiteralPath (Join-Path $repo 'frontends') -Filter frontend.json -Recurse -File)
    foreach ($family in @('canoe', 'pcanview')) {
        $metadata = @($descriptors | Where-Object {
            (Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).id -eq $family
        })
        if ($metadata.Count -ne 1) { throw "Reference frontend metadata missing or ambiguous: $family" }
        $projects = @(Get-ChildItem -LiteralPath $metadata[0].DirectoryName -Filter '*.pro' -File)
        if ($projects.Count -ne 1) { throw "Reference frontend project missing or ambiguous: $family" }
        $build = Build-QtProject "frontend-$family" $projects[0].FullName
        $folder = Join-Path $frontendRoot $family
        $library = Join-Path $folder $Architecture
        New-Item -ItemType Directory -Force -Path $library | Out-Null
        Copy-Item -LiteralPath $metadata[0].FullName -Destination $folder
        $description = Get-Content -LiteralPath $metadata[0].FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $dllName = [IO.Path]::GetFileName(([string]$description.library).Replace('{architecture}', $Architecture))
        Copy-Item -LiteralPath (Join-Path $build ('bin/' + $dllName)) -Destination $library
    }
    $env:UCAL_FRONTEND_DIRECTORY = $frontendRoot
    $app = Build-QtProject 'product' (Join-Path $repo 'manager/UcalManager.pro')
    $env:UCAL_MANAGER_ACCEPTANCE_EXE = Join-Path $app 'bin/UcalManager.exe'
    $tests = Build-QtProject 'tests' (Join-Path $repo 'manager/test/ManagerModelTests.pro')
    $test = Join-Path $tests 'bin/UcalManagerModelTests.exe'
    $null = Invoke-UcalLoggedProcess $test @('-functions') $output (Join-Path $output 'functions.txt')
    $functions = @(Get-Content (Join-Path $output 'functions.txt') | Where-Object { $_ -match '^\w+\(\)$' } | ForEach-Object { $_ -replace '\(\)$', '' })
    $catalog = Get-Content -LiteralPath (Join-Path $repo 'manager/test/cases.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $expected = @($catalog.cases | ForEach-Object { $_.id })
    if ($catalog.caseCount -ne $expected.Count -or $functions.Count -ne $expected.Count -or
        @(Compare-Object $expected $functions -CaseSensitive).Count) {
        throw 'Qt function discovery differs from test/cases.json. Update the maintained case catalog with the test change.'
    }
    $summary.caseCount = $expected.Count
    $xmlPath = Join-Path $output 'results.xml'
    Write-Host "Run $($functions.Count) Qt cases ($($summary.platform))"
    $code = Invoke-UcalLoggedProcess $test @('-maxwarnings', '0', '-o', ((Join-Path $output 'results.txt') + ',txt'),
        '-o', ($xmlPath + ',xml')) $output `
        (Join-Path $output 'test-process.log') -TimeoutSeconds $TestTimeoutSeconds -AllowFailure
    $summary.processExitCode = $code
    try { $records = @(Read-UcalQtResults $xmlPath) } catch {
        throw "Qt process exit code $code; its XML report is incomplete or invalid. See results.txt and test-process.log."
    }
    Write-UcalQtJUnit $records (Join-Path $output 'junit.xml')
    $summary.cases = $records
    if (@(Compare-Object (@($expected) + @('initTestCase', 'cleanupTestCase')) @($records | ForEach-Object { $_.id }) -CaseSensitive).Count) {
        throw 'The result does not contain the complete maintained suite, including its setup and cleanup.'
    }
    $allowedSkips = @($catalog.cases | Where-Object {
        $_.skipPolicy -eq 'no-system-tray'
    } | ForEach-Object { $_.id })
    $summary.skipped = @($records | Where-Object { $_.status -eq 'skipped' } | ForEach-Object { $_.id })
    $summary.passedCaseCount = @($records | Where-Object { $_.id -in $expected -and $_.status -eq 'passed' }).Count
    if ($code -ne 0 -or @($records | Where-Object { $_.status -eq 'failed' }).Count -or
        @($summary.skipped | Where-Object { $_ -notin $allowedSkips }).Count) {
        throw 'Qt regression failed or unexpectedly skipped a required case. See results.txt and summary.json.'
    }
    $summary.status = 'passed'
    Write-Host "Suite passed: $($summary.passedCaseCount) cases passed; platform skips: $(@($summary.skipped).Count). Results: $output"
} catch {
    $summary.error = $_.Exception.Message
    throw
} finally {
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'), ($summary | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    Get-ChildItem Env: | Where-Object { -not $saved.ContainsKey($_.Name) } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
