[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageRoot,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [string]$FixtureRoot,
      [string]$ProbePath,
      [string]$BackendRuntimeOverridesPath)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$root=(Resolve-Path -LiteralPath $PackageRoot).Path
if(-not $FixtureRoot){$FixtureRoot=Join-Path $repo 'out/x64/Release'}
if(-not $ProbePath){$ProbePath=Join-Path $FixtureRoot 'UcalHarness.exe'}
$probe=(Resolve-Path -LiteralPath $ProbePath).Path
$pcan=(Resolve-Path -LiteralPath (Join-Path $FixtureRoot 'UcalPcanBasicTestRuntime.dll')).Path
$vector=(Resolve-Path -LiteralPath (Join-Path $FixtureRoot 'UcalVectorXlTestRuntime.dll')).Path
$fixtures=@{ 'pcan-basic'=$pcan; 'vector-xl'=$vector }
if($BackendRuntimeOverridesPath){
    $overrides=Get-Content -LiteralPath $BackendRuntimeOverridesPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($entry in $overrides.PSObject.Properties){
        if($entry.Name -notmatch '^[a-z0-9-]+$' -or
            -not(Test-Path -LiteralPath (Join-Path $root "backends/$($entry.Name)/backend.json"))){throw 'Unknown runtime override backend.'}
        $fixtures[$entry.Name]=(Resolve-Path -LiteralPath ([string]$entry.Value)).Path
    }
}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$broker=Join-Path $root 'runtime/x64/UcalBroker.exe'
$worker32=Join-Path $root 'runtime/x86/UcalBroker.exe'
$program=Join-Path $root 'UcalManager.exe'
$brokerPaths=@($broker,$worker32)
$before=@(Get-CimInstance Win32_Process -Filter "Name='UcalBroker.exe'" | Where-Object {$_.ExecutablePath -in $brokerPaths})
if($before.Count){throw 'A process from this exact package is already running; use a fresh package copy.'}
$saved=@{}
Get-ChildItem Env: | Where-Object {$_.Name -like 'UCAL_*'} | ForEach-Object {
    $saved[$_.Name]=$_.Value; [Environment]::SetEnvironmentVariable($_.Name,$null,'Process')
}
$application=$null; $worker=$null
try{
    $settings=Join-Path $output 'data'
    $backends=Join-Path $settings 'backend-plugins'
    New-Item -ItemType Directory -Force -Path $backends | Out-Null
    foreach($id in $fixtures.Keys){
        [IO.File]::WriteAllText((Join-Path $backends ($id+'.ini')),"[runtime]`r`nlibrary=$($fixtures[$id])`r`n",[Text.Encoding]::Unicode)
    }
    $env:UCAL_MANAGER_SETTINGS_DIRECTORY=$settings
    $env:UCAL_BROKER_INSTANCE='release-cold-'+[guid]::NewGuid().ToString('N')
    $screenshot=Join-Path $output 'hardware.png'
    $workspace=Join-Path $settings 'workspace.json'
    $application=Start-Process -FilePath $program -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -ArgumentList @('--presentation','production','--language','en','--live','--workspace',('"'+$workspace+'"'),'--page','hardware','--screenshot',('"'+$screenshot+'"'))
    $health=@()
    for($attempt=0;$attempt -lt 80;$attempt++){
        $raw=@(& $probe backend-health 2>$null)
        if($LASTEXITCODE -eq 0){
            $health=@($raw | Where-Object {$_ -like '{*'} | ForEach-Object {$_ | ConvertFrom-Json})
            if(@($health | Where-Object {$_.status -eq 0 -and $_.endpointCount -gt 0}).Count -eq $fixtures.Count){break}
        }
        Start-Sleep -Milliseconds 100
    }
    [IO.File]::WriteAllText((Join-Path $output 'backend-health.json'),($health | ConvertTo-Json -Depth 7),[Text.UTF8Encoding]::new($false))
    $owned=@(Get-CimInstance Win32_Process -Filter "Name='UcalBroker.exe'" | Where-Object {$_.ExecutablePath -eq $broker})
    if(-not $owned.Count){throw 'Manager did not start the Broker from its own package.'}
    # Generic plugin health identifies the UCAL adapter. Verify the selected
    # lower-level runtime using the modules actually loaded by our own workers.
    $loaded=@($owned | ForEach-Object { (Get-Process -Id $_.ProcessId).Modules | ForEach-Object {$_.FileName} })
    foreach($library in $fixtures.Values){
        if($library -notin $loaded){throw "Manager settings did not select the fixture runtime: $library"}
    }
    foreach($family in $fixtures.Keys){
        if(@($health | Where-Object {$_.backendFamily -eq $family -and $_.status -eq 0 -and $_.endpointCount -gt 0}).Count -ne 1){throw "Fixture backend failed to enumerate: $family"}
    }
    if(-not $application.WaitForExit(15000) -or $application.ExitCode -ne 0 -or -not(Test-Path -LiteralPath $screenshot)){throw 'User package UI/render/exit failed.'}
    # No developer probe is included in the package, but the x86 worker must still
    # start on its own matching runtime. Query it using the source-built probe.
    $env:UCAL_BROKER_INSTANCE='release-worker32-'+[guid]::NewGuid().ToString('N')
    $worker=Start-Process -FilePath $worker32 -WorkingDirectory (Split-Path $worker32 -Parent) -ArgumentList '--allow-test-shutdown' -WindowStyle Hidden -PassThru
    $ready=$false
    for($attempt=0;$attempt -lt 40;$attempt++){
        & $probe ping *> $null
        if($LASTEXITCODE -eq 0){$ready=$true;break}; Start-Sleep -Milliseconds 100
    }
    if(-not $ready){throw 'Packaged x86 worker did not answer the source-built probe.'}
    & $probe shutdown *> $null
    if($LASTEXITCODE -ne 0 -or -not $worker.WaitForExit(5000) -or $worker.ExitCode -ne 0){throw 'Packaged x86 worker shutdown failed.'}
    $result=[ordered]@{schema='org.ucal.user-release-test/1';status='passed';hardwareAccess=$false;
        managerStartsOwnBroker=$true;nativeRuntimeSettings=$true;fixtureBackends=$health;
        x86WorkerStartup=$true;developerToolsInPackage=$false}
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'),($result | ConvertTo-Json -Depth 7),[Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Depth 7
}finally{
    if($application -and -not $application.HasExited){Stop-Process -Id $application.Id}
    if($worker -and -not $worker.HasExited){Stop-Process -Id $worker.Id}
    # The initial guard verified there were no running processes from either
    # executable in this exact package. Terminate only this test's new copies.
    foreach($process in @(Get-CimInstance Win32_Process -Filter "Name='UcalBroker.exe'" | Where-Object {$_.ExecutablePath -in $brokerPaths})){
        Stop-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
    }
    Get-ChildItem Env: | Where-Object {$_.Name -like 'UCAL_*'} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
$global:LASTEXITCODE=0
