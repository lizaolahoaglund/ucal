[CmdletBinding()]
param(
    [string]$QmakePath=$env:UCAL_QMAKE,
    [string]$MakePath,
    [string]$MSBuildPath=$env:UCAL_MSBUILD,
    [string]$BuildDirectory,
    [string]$DistDirectory,
    [Parameter(Mandatory=$true)][string]$RuntimeNoticesRoot,
    [string]$PcanViewNativeRoot,
    [string]$SourceArchivePath,
    [string]$SourceRevision,
    [switch]$IncludePrivate,
    [switch]$SkipNative,
    [switch]$PublicOnly
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $repo 'tools/UcalToolchain.psm1') -Force
if(-not $QmakePath){$command=Get-Command qmake.exe -ErrorAction SilentlyContinue;if($command){$QmakePath=$command.Source}}
if(-not $QmakePath){throw 'Pass -QmakePath or set UCAL_QMAKE to the Qt 5.14.2 MinGW x64 shipping kit.'}
$qmake=(Resolve-Path -LiteralPath $QmakePath).Path
if((& $qmake -query QT_VERSION) -ne '5.14.2' -or (& $qmake -query QMAKE_XSPEC) -ne 'win32-g++'){
    throw 'Portable distribution uses the maintained Qt 5.14.2 MinGW x64 kit; the regression suite also tests MSVC x86/x64.'
}
$qtBin=Split-Path $qmake -Parent
$qtRoot=Split-Path $qtBin -Parent
if(-not $MakePath){$command=Get-Command mingw32-make.exe -ErrorAction SilentlyContinue;if($command){$MakePath=$command.Source}}
if(-not $MakePath){throw 'Pass -MakePath for the compiler/make toolchain matching the selected Qt kit.'}
$make=(Resolve-Path -LiteralPath $MakePath).Path
if(-not $BuildDirectory){$BuildDirectory=Join-Path $repo 'out/dist-build'}
if(-not $DistDirectory){$DistDirectory=Join-Path $repo 'dist'}
$build=[IO.Path]::GetFullPath($BuildDirectory)
$dist=[IO.Path]::GetFullPath($DistDirectory)
New-Item -ItemType Directory -Force $build | Out-Null
$saved=@{};Get-ChildItem Env: | ForEach-Object {$saved[$_.Name]=$_.Value}
try{
    $msbuild=Get-UcalMSBuildPath $MSBuildPath
    if(-not $SkipNative){
        foreach($platform in @('x64','Win32')){
            $null=Invoke-UcalLoggedProcess $msbuild @((Join-Path $repo 'Ucal.sln'),'/m','/nologo','/verbosity:minimal','/p:Configuration=Release',"/p:Platform=$platform") $repo (Join-Path $build "native-$platform.log") -TimeoutSeconds 900
            & (Join-Path $PSScriptRoot 'Build-UcalPlugins.ps1') -Configuration Release -Platform $platform -MSBuildPath $msbuild -IncludePrivate:($IncludePrivate -and -not $PublicOnly)
        }
    }
    $env:PATH=$qtBin+';'+(Split-Path $make -Parent)+';'+$env:PATH
    $productBuild=Join-Path $build 'manager'
    New-Item -ItemType Directory -Force $productBuild | Out-Null
    $null=Invoke-UcalLoggedProcess $qmake @((Join-Path $repo 'manager/UcalManager.pro'),'CONFIG+=release','CONFIG-=debug') $productBuild (Join-Path $build 'manager-qmake.log') -TimeoutSeconds 60
    $null=Invoke-UcalLoggedProcess $make @('-j2') $productBuild (Join-Path $build 'manager-build.log') -TimeoutSeconds 900
    $frontendBuild=Join-Path $build 'software-plugins'
    & (Join-Path $PSScriptRoot 'Build-UcalManagerFrontends.ps1') -QmakePath $qmake -MakePath $make -OutputRoot $frontendBuild -IncludePrivate:($IncludePrivate -and -not $PublicOnly)
    $deployment=Join-Path $build 'qt-deployment'
    New-Item -ItemType Directory -Force $deployment | Out-Null
    Copy-Item -LiteralPath (Join-Path $productBuild 'bin/UcalManager.exe') -Destination $deployment -Force
    # Copy the selected kit's runtime explicitly, including the required platform
    # plugins. This also works with kits whose windeployqt misclassifies plugins.
    foreach($name in @('Qt5Core.dll','Qt5Gui.dll','Qt5Network.dll','Qt5Widgets.dll','Qt5Concurrent.dll','libEGL.dll','libGLESv2.dll','D3Dcompiler_47.dll')){
        Copy-Item -LiteralPath (Join-Path $qtBin $name) -Destination $deployment -Force
    }
    foreach($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')){
        Copy-Item -LiteralPath (Join-Path (Split-Path $make -Parent) $name) -Destination $deployment -Force
    }
    foreach($kind in @('platforms','styles','imageformats')){
        $target=Join-Path $deployment $kind;New-Item -ItemType Directory -Force $target | Out-Null
        $names=if($kind -eq 'platforms'){@('qwindows.dll')}elseif($kind -eq 'styles'){@('qwindowsvistastyle.dll')}else{@('qico.dll','qjpeg.dll')}
        foreach($name in $names){Copy-Item -LiteralPath (Join-Path $qtRoot "plugins/$kind/$name") -Destination $target -Force}
    }
    $packageInputs=@{ManagerDeploymentRoot=$deployment;PackageRoot=$dist;FrontendPluginRoot=(Join-Path $frontendBuild 'frontends');
        RuntimeNoticesRoot=$RuntimeNoticesRoot;PublicOnly=$PublicOnly;IncludePrivate=$IncludePrivate;CorrespondingSourceDirectory=(Join-Path $build 'artifacts')}
    foreach($key in @('PcanViewNativeRoot','SourceArchivePath','SourceRevision')){if(Get-Variable -Name $key -ValueOnly){$packageInputs[$key]=Get-Variable -Name $key -ValueOnly}}
    & (Join-Path $PSScriptRoot 'packaging/New-UcalManagerPackage.ps1') @packageInputs
    # Broker/worker binaries use the Microsoft C++ runtime. Keep their architecture
    # matching dependencies beside the executables in the generated product.
    $visualStudio=Find-UcalVisualStudio
    $redists=@(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Redist/MSVC') -Directory | Where-Object {$_.Name -match '^\d+\.'} | Sort-Object {[version]$_.Name} -Descending)
    if(-not $redists.Count){throw 'Microsoft C++ redistributable build inputs were not found.'}
    & (Join-Path $PSScriptRoot 'packaging/Copy-UcalCrtDependencies.ps1') -PackageRoot $dist -RedistRoot $redists[0].FullName
    & (Join-Path $PSScriptRoot 'packaging/New-UcalManagerPackageManifest.ps1') -PackageRoot $dist -Version '0.9.1'
    Write-Host "Complete portable product: $dist"
}finally{
    Get-ChildItem Env: | Where-Object {-not $saved.ContainsKey($_.Name)} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
