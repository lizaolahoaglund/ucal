[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$QmakePath,
    [string]$MSBuildPath=$env:UCAL_MSBUILD,
    [string]$VcVarsAllPath=$env:UCAL_VCVARSALL,
    [string]$OutputDirectory,
    [string[]]$Case
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot '../tools/UcalToolchain.psm1') -Force
$msbuild=Get-UcalMSBuildPath $MSBuildPath
$null=Import-UcalMsvcEnvironment x64 $VcVarsAllPath
$qmake=(Resolve-Path -LiteralPath $QmakePath).Path
$make=(Get-Command nmake.exe -ErrorAction Stop).Source
$groups=@(@('frontends','backends') | ForEach-Object {
    Get-ChildItem -LiteralPath (Join-Path $repo "$_") -Directory | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName 'plugin-package.json') -PathType Leaf
    }
})
$removals=@{}
foreach($group in $groups) {
    $category=Split-Path (Split-Path $group.FullName -Parent) -Leaf
    $removals["without-$category-$($group.Name)"]=@("$category/$($group.Name)/")
}
if(-not $Case){$Case=@('ucal-only','core-only')+@($removals.Keys | Sort-Object)+@('standalone-backends','standalone-frontends')}
foreach($id in $Case){if($id -notin @('ucal-only','core-only','standalone-backends','standalone-frontends') -and -not $removals.ContainsKey($id)){throw "Unknown isolation case: $id"}}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo ('out/source-isolation-'+[Guid]::NewGuid().ToString('N'))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new or empty output directory.'}
New-Item -ItemType Directory -Force $output | Out-Null
if(Test-Path -LiteralPath (Join-Path $repo '.git')){
    & git -C $repo diff --quiet HEAD -- . ":(exclude)AGENTS.md"
    if($LASTEXITCODE){throw 'Commit the source under test before the clean-source proof.'}
    $sourceFiles=@(& git -C $repo ls-files)
}else{
    $sourceFiles=@(Get-ChildItem -LiteralPath $repo -Recurse -File | Where-Object {
        $_.FullName.Substring($repo.Length+1) -notmatch '(^|[\\/])(out|dist|build|\.git|\.vs|data)([\\/]|$)'
    } | ForEach-Object {$_.FullName.Substring($repo.Length+1)})
}
$results=@()
try{
 foreach($id in $Case){
    $null=Import-UcalMsvcEnvironment x64 $VcVarsAllPath
    $snapshot=Join-Path $output "$id/source tree"
    New-Item -ItemType Directory -Force $snapshot | Out-Null
    $omit=if($id -eq 'core-only'){@('frontends/','backends/')}elseif($removals.ContainsKey($id)){$removals[$id]}else{@()}
    foreach($relative in $sourceFiles){
        $normalized=$relative.Replace('\','/')
        if($id -eq 'ucal-only' -and -not($normalized.StartsWith('ucal/') -or $normalized.StartsWith('ucal/test/'))){continue}
        $excluded=$false
        foreach($prefix in $omit){if($normalized.StartsWith($prefix)){$excluded=$true;break}}
        if($excluded){continue}
        $target=Join-Path $snapshot $relative
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        if ($relative -eq 'AGENTS.md') {
            & git -C $repo show 'HEAD:AGENTS.md' | Set-Content -LiteralPath $target -Encoding UTF8
        } else { Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $target }
    }
    $caseRoot=Split-Path $snapshot -Parent
    $ucalRoot=(Join-Path $snapshot 'ucal').Replace('\','/')+'/'
    if($id -eq 'ucal-only'){
        foreach($platform in @('x64','Win32')){
            $null=Invoke-UcalLoggedProcess $msbuild @((Join-Path $ucalRoot 'Ucal.vcxproj'),'/nologo','/verbosity:minimal','/p:ImportDirectoryBuildProps=false','/p:Configuration=Release',"/p:Platform=$platform") $snapshot (Join-Path $caseRoot "library-$platform.log") -TimeoutSeconds 300
            $testOutput=(Join-Path $caseRoot "c-$platform").Replace('\','/')+'/'
            $null=Invoke-UcalLoggedProcess $msbuild @((Join-Path $snapshot 'ucal/test/UcalCTest.vcxproj'),'/nologo','/verbosity:minimal','/p:ImportDirectoryBuildProps=false','/p:Configuration=Release',"/p:Platform=$platform","/p:OutDir=$testOutput") $snapshot (Join-Path $caseRoot "c-$platform.log") -TimeoutSeconds 300
            $null=Invoke-UcalLoggedProcess (Join-Path $testOutput 'UcalCTest.exe') @() $snapshot (Join-Path $caseRoot "c-$platform-run.log") -TimeoutSeconds 30
        }
        & (Join-Path $PSScriptRoot '../ucal/test/Test-UcalTools.ps1') -UcalRoot $ucalRoot -FixtureDirectory (Join-Path $snapshot 'ucal/test') -OutputDirectory (Join-Path $caseRoot 'ucal-tools') -VcVarsAllPath $VcVarsAllPath
    }elseif($id -like 'standalone-*'){
        $categories=if($id -eq 'standalone-backends'){@('backends')}else{@('frontends')}
        foreach($group in @($groups | Where-Object {(Split-Path (Split-Path $_.FullName -Parent) -Leaf) -in $categories})){
            $category=Split-Path (Split-Path $group.FullName -Parent) -Leaf
            $external=Join-Path $caseRoot ('external '+$group.Name)
            Copy-Item -LiteralPath (Join-Path $snapshot "$category/$($group.Name)") -Destination $external -Recurse
            foreach($platform in @('x64','Win32')){
                foreach($project in Get-ChildItem -LiteralPath $external -Filter '*.vcxproj' -File){
                    [xml]$definition=Get-Content -LiteralPath $project.FullName -Raw -Encoding UTF8
                    if(@($definition.SelectNodes("//*[local-name()='ConfigurationType']") | Where-Object {$_.InnerText -eq 'Makefile'}).Count){continue}
                    if(-not @($definition.SelectNodes("//*[local-name()='ProjectConfiguration']") | Where-Object {$_.Include -eq "Release|$platform"}).Count){continue}
                    $null=Invoke-UcalLoggedProcess $msbuild @($project.FullName,'/nologo','/verbosity:minimal','/p:ImportDirectoryBuildProps=false','/p:Configuration=Release',"/p:Platform=$platform","/p:UcalRoot=$ucalRoot") $external (Join-Path $caseRoot ($project.BaseName+"-$platform.log")) -TimeoutSeconds 300
                }
            }
            if($category -eq 'frontends'){
                foreach($project in Get-ChildItem -LiteralPath $external -Filter '*.pro' -File){
                    $build=Join-Path $caseRoot ('software '+$group.Name)
                    New-Item -ItemType Directory -Force $build | Out-Null
                    $null=Invoke-UcalLoggedProcess $qmake @($project.FullName,'CONFIG+=release','CONFIG-=debug',('UCAL_SOURCE_ROOT="'+$snapshot.Replace('\','/')+'"')) $build (Join-Path $caseRoot ($group.Name+'-qmake.log')) -TimeoutSeconds 60
                    $null=Invoke-UcalLoggedProcess $make @('/NOLOGO') $build (Join-Path $caseRoot ($group.Name+'-software.log')) -TimeoutSeconds 600
                }
            }
        }
    }else{
        $null=Invoke-UcalLoggedProcess $msbuild @((Join-Path $snapshot 'Ucal.sln'),'/m','/nologo','/verbosity:minimal','/p:Configuration=Release','/p:Platform=x64') $snapshot (Join-Path $caseRoot 'core.log') -TimeoutSeconds 600
        $build=Join-Path $caseRoot 'manager build'
        New-Item -ItemType Directory -Force $build | Out-Null
        $null=Invoke-UcalLoggedProcess $qmake @((Join-Path $snapshot 'manager/UcalManager.pro'),'CONFIG+=release','CONFIG-=debug') $build (Join-Path $caseRoot 'manager-qmake.log') -TimeoutSeconds 60
        $null=Invoke-UcalLoggedProcess $make @('/NOLOGO') $build (Join-Path $caseRoot 'manager.log') -TimeoutSeconds 600
        if($id -ne 'core-only'){
            $powershell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
            $null=Invoke-UcalLoggedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $snapshot 'tools/Build-UcalPlugins.ps1'),'-Configuration','Release','-Platform','x64','-IncludePrivate','-MSBuildPath',$msbuild) $snapshot (Join-Path $caseRoot 'native.log') -TimeoutSeconds 900
            $null=Invoke-UcalLoggedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $snapshot 'tools/Build-UcalManagerFrontends.ps1'),'-QmakePath',$qmake,'-OutputRoot',(Join-Path $caseRoot 'frontend packages'),'-MakePath',$make) $snapshot (Join-Path $caseRoot 'frontends.log') -TimeoutSeconds 900
        }
    }
    $results+=[ordered]@{id=$id;status='passed';sourceOnly=$true;pathContainsSpaces=$true}
    Write-Host "Isolation case passed: $id"
 }
}finally{
 $status=if($results.Count -eq $Case.Count){'passed'}else{'failed'}
 [IO.File]::WriteAllText((Join-Path $output 'summary.json'),([ordered]@{schema='org.ucal.source-isolation/2';status=$status;hardwareAccess=$false;cases=@($results)} | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
}
