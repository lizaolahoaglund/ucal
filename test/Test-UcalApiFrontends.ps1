[CmdletBinding()]
param([string]$OutputDirectory, [string]$MSBuildPath=$env:UCAL_MSBUILD, [switch]$SkipBuild)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $repo 'tools/UcalToolchain.psm1') -Force
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo ('out/api-frontends/'+[guid]::NewGuid().ToString('N'))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new or empty evidence directory.'}
New-Item -ItemType Directory -Force $output | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object {$_.Name -like 'UCAL_*'} | ForEach-Object {
    $saved[$_.Name]=$_.Value
    [Environment]::SetEnvironmentVariable($_.Name,$null,'Process')
}
$results=@(); $transactions=@(); $expected=0; $completed=$false
try{
    if(-not $SkipBuild){
        $msbuild=Get-UcalMSBuildPath $MSBuildPath
        foreach($arch in @('x64','Win32')){
            $null=Invoke-UcalLoggedProcess $msbuild @((Join-Path $repo 'Ucal.sln'),'/m','/nologo','/verbosity:minimal','/p:Configuration=Release',"/p:Platform=$arch") $repo (Join-Path $output "core-$arch.log") -TimeoutSeconds 600
            & (Join-Path $repo 'tools/Build-UcalPlugins.ps1') -Configuration Release -Platform $arch -IncludeTests -IncludePrivate -MSBuildPath $msbuild
            if($LASTEXITCODE){throw "Native API client build failed: $arch"}
        }
    }
    $bin=Join-Path $repo 'out/x64/Release'
    $broker=Join-Path $bin 'UcalBroker.exe'; $harness=Join-Path $bin 'UcalHarness.exe'
    $powershell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $backends=@('pcan','vector','j2534')
    $profiles=@(
        @{id='vxlapi';client='UcalVxlApiClient.exe';script='Set-UcalVxlClientDeployment.ps1'},
        @{id='pcan_basic';client='UcalPcanBasicClient.exe';script='Set-UcalPcanBasicClientDeployment.ps1'}
    ) | Where-Object {Test-Path -LiteralPath (Join-Path $repo "frontends/$($_.id)/plugin-package.json")}
    $expected=@($profiles).Count*2*$backends.Count
    if(-not $expected){throw 'No API frontend source group was selected.'}
    $catalog=Get-Content -LiteralPath (Join-Path $repo 'test/cases.json') -Raw | ConvertFrom-Json
    $specifications=@($catalog.cases | Where-Object {$_.api -in $profiles.id})
    if(@($catalog.cases).Count -ne $catalog.caseCount -or @($specifications | Where-Object {$_.kind -eq 'route'}).Count -ne $expected){throw 'API case catalog does not match the selected matrix.'}
    foreach($api in $profiles){
        $descriptor=Get-Content -LiteralPath (Join-Path $repo "frontends/$($api.id)/deployment.json") -Raw | ConvertFrom-Json
        $script=Join-Path $repo "frontends/$($api.id)/tools/$($api.script)"
        foreach($arch in @('x64','x86')){
            $nativeArch=if($arch -eq 'x86'){'Win32'}else{'x64'}
            $native=Join-Path $repo "out/$nativeArch/Release"
            $contract=$descriptor.architectures.$arch
            $clientRoot=Join-Path $output "$($api.id)-$arch/client"
            New-Item -ItemType Directory -Force $clientRoot | Out-Null
            $exe=Join-Path $clientRoot $api.client
            Copy-Item -LiteralPath (Join-Path $native $api.client) -Destination $exe
            $originalExeHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
            $target=Join-Path $clientRoot $contract.import_name
            [IO.File]::WriteAllText($target,'original owned application-local placeholder')
            $originalDllHash=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            $backup=Join-Path $output "$($api.id)-$arch/backup"
            $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$script,'-Action','Apply',
                '-TargetExecutable',$exe,'-FrontendDll',(Join-Path $native $contract.import_name),'-BackupDirectory',$backup)
            if($api.id -eq 'vxlapi'){$arguments+=@('-DiscoveryDll',(Join-Path $native $contract.files[1].name))}
            $null=Invoke-UcalLoggedProcess $powershell $arguments $repo (Join-Path $output "$($api.id)-$arch/apply.log") -TimeoutSeconds 30
            foreach($backend in $backends){
                $case="$($api.id)-$arch-$backend"
                $caseRoot=Join-Path $output $case
                New-Item -ItemType Directory -Force $caseRoot | Out-Null
                Get-ChildItem Env: | Where-Object {$_.Name -like 'UCAL_*'} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
                $env:UCAL_BROKER_INSTANCE='api_'+[guid]::NewGuid().ToString('N')
                $env:UCAL_BACKEND=$backend
                switch($backend){
                    'pcan' {$env:UCAL_PCAN_BASIC_RUNTIME=Join-Path $bin 'UcalPcanBasicTestRuntime.dll';$env:UCAL_PCAN_ALLOW_ECHO_FRAMES='1'}
                    'vector' {$env:UCAL_VECTOR_XL_RUNTIME=Join-Path $bin 'UcalVectorXlTestRuntime.dll'}
                    'j2534' {
                        $env:UCAL_J2534_CONFIG=Join-Path $caseRoot 'provider.ini'
                        $env:UCAL_J2534_WORKER_PATH=$broker
                        $env:UCAL_J2534_TEST_CASE='normal'
                        @"
[provider]
profile=04.04-gmw17753-raw-iso-fd
id=original_api_test
runtime=$(Join-Path $bin 'UcalJ2534TestRuntime.dll')
endpoint_count=1
pins=1550
[endpoint.0]
selector=fixture-device:0
adapter_identity=fixture-device
physical_port=0
name=Original API J2534 test channel
model=Original test
serial=original-test
brs=1
termination=1
"@ | Set-Content -LiteralPath $env:UCAL_J2534_CONFIG -Encoding Unicode
                    }
                }
                $process=Start-Process -FilePath $broker -ArgumentList '--allow-test-shutdown' -WindowStyle Hidden -PassThru `
                    -RedirectStandardOutput (Join-Path $caseRoot 'broker.stdout.log') -RedirectStandardError (Join-Path $caseRoot 'broker.stderr.log')
                try{
                    $ready=$false
                    for($i=0;$i -lt 50;$i++){
                        & $harness ping *> (Join-Path $caseRoot 'ping.log')
                        if($LASTEXITCODE -eq 0){$ready=$true;break}
                        $process.Refresh();if($process.HasExited){break}
                        Start-Sleep -Milliseconds 50
                    }
                    if(-not $ready){throw "API test Broker not ready: $case"}
                    $log=Join-Path $caseRoot 'client.log'
                    $fdSamplingRejected=$api.id -eq 'pcan_basic' -and $backend -eq 'j2534'
                    $spec=@($specifications | Where-Object {$_.id -eq $case})
                    if($spec.Count -ne 1 -or $spec[0].expectedFdSamplingRejection -ne $fdSamplingRejected){throw "API scenario is absent from the maintained catalog: $case"}
                    $clientArguments=if($fdSamplingRejected){@('--expect-fd-unsupported')}elseif($backend -eq 'j2534'){@('--rates-only')}else{@()}
                    $null=Invoke-UcalLoggedProcess $exe $clientArguments $clientRoot $log -TimeoutSeconds 15
                    $text=Get-Content -LiteralPath $log -Raw
                    $expectedText=if($fdSamplingRejected){'*classic=true fdSamplingRejected=true staticImport=true*'}else{'*classic=true fd=true brs=true staticImport=true*'}
                    if($text -notlike $expectedText -or
                        $text.IndexOf($target,[StringComparison]::OrdinalIgnoreCase) -lt 0){throw "API route/module evidence missing: $case"}
                    & $harness shutdown *> (Join-Path $caseRoot 'shutdown.log')
                    if($LASTEXITCODE -ne 0 -or -not $process.WaitForExit(5000)){throw "API Broker clean shutdown failed: $case"}
                    $results+=[ordered]@{id=$case;status='passed';clientArchitecture=$arch;brokerArchitecture='x64';backend=$backend;classic=$true;canFd=(-not $fdSamplingRejected);brs=(-not $fdSamplingRejected);fdSamplingRejected=$fdSamplingRejected;ratesOnly=($backend -eq 'j2534' -and -not $fdSamplingRejected);adjacentModule=$true;cleanShutdown=$true}
                    Write-Host "API frontend route passed: $case"
                }finally{
                    $process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id}
                    $process.Dispose()
                }
            }
            $null=Invoke-UcalLoggedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',$script,'-Action','Restore',
                '-TargetExecutable',$exe,'-BackupDirectory',$backup) $repo (Join-Path $output "$($api.id)-$arch/restore.log") -TimeoutSeconds 30
            if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $originalExeHash -or
               (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $originalDllHash){throw 'API deployment did not restore exact original files.'}
            $transactions+=[ordered]@{id="$($api.id)-$arch-deployment-restore";api=$api.id;architecture=$arch;status='passed';executableUnchanged=$true;exactRestore=$true}
        }
    }
    $completed=$results.Count -eq $expected -and $transactions.Count -eq @($profiles).Count*2
    $actualIds=@(@($results)+@($transactions) | ForEach-Object {$_.id} | Sort-Object)
    if(Compare-Object @($specifications.id | Sort-Object) $actualIds){$completed=$false}
    if(-not $completed){throw 'Incomplete API frontend route matrix.'}
}finally{
    Get-ChildItem Env: | Where-Object {$_.Name -like 'UCAL_*'} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'),([ordered]@{
        schema='org.ucal.api-frontend-regression/1';status=$(if($completed){'passed'}else{'failed'});hardwareAccess=$false;
        expectedCases=$expected;cases=$results;deploymentTransactions=$transactions
    } | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
}
