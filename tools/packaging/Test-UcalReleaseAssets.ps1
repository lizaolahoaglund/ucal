[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageRoot,
      [ValidateSet('community','internal')][string]$Edition='community')
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $PackageRoot).Path
$source=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $source 'ucal/tools/UcalPeContract.psm1') -Force
$expected=@('UcalManager.exe','Qt5Core.dll','Qt5Gui.dll','Qt5Widgets.dll','Qt5Network.dll','Qt5Concurrent.dll',
    'libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll','libEGL.dll','libGLESv2.dll','D3Dcompiler_47.dll',
    'platforms/qwindows.dll','styles/qwindowsvistastyle.dll','imageformats/qico.dll','imageformats/qjpeg.dll')
foreach($arch in @('x64','x86')){
    foreach($name in @('UcalBroker.exe','UcalBackendJ2534.dll','UcalJ2534DiscoveryHost.exe','ucal_native_backends.ini')){$expected+="runtime/$arch/$name"}
}
foreach($category in @('frontends','backends')){
    foreach($descriptor in Get-ChildItem -LiteralPath (Join-Path $source $category) -Filter plugin-package.json -Recurse -File){
        $module=Get-Content -LiteralPath $descriptor.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $private=$module.visibility -eq 'private'
        if($private -and $Edition -ne 'internal'){continue}
        foreach($asset in @($module.runtime_files)){
            $parts=([string]$asset.destination).Replace('\','/').Split('/')
            if($private -or $parts[0] -eq 'runtime' -or (Test-Path -LiteralPath (Join-Path $root ($parts[0]+'/'+$parts[1])))){$expected+=[string]$asset.destination}
        }
        foreach($name in @($module.backends)){
            $metadata=Get-Content -LiteralPath (Join-Path $descriptor.DirectoryName $name) -Raw -Encoding UTF8 | ConvertFrom-Json
            if(-not $private -and -not(Test-Path -LiteralPath (Join-Path $root "backends/$($metadata.id)/backend.json"))){continue}
            $expected+="backends/$($metadata.id)/backend.json"
            foreach($arch in @('x64','x86')){$expected+="backends/$($metadata.id)/$arch/$([IO.Path]::GetFileName($metadata.library))"}
        }
    }
}
foreach($relative in $expected | Sort-Object -Unique){
    if(-not(Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)){throw "Required release runtime asset is missing: $relative"}
}
foreach($file in Get-ChildItem -LiteralPath $root -Recurse -File){
    $relative=$file.FullName.Substring($root.TrimEnd('\').Length+1).Replace('\','/')
    if($relative -in @('licenses/GPL-3.0.txt','licenses/Independent-modules-exception.txt','licenses/CompatibilitySupport.txt','licenses/ThirdPartyNotices.txt')){continue}
    if($relative -match '(^|/)(tools|compatibility|resources|include|sdk|test)(/|$)' -or
       $file.Extension -in @('.ps1','.psm1','.psd1','.h','.hpp','.c','.cpp','.lib','.pdb','.tar','.zip') -or
       $file.Name -eq 'UcalHarness.exe') {throw "Developer or SDK payload is not allowed in the user release: $relative"}
    if($file.Extension -notin @('.dll','.exe')){
        if($relative -in $expected -or $relative -in @('README.md','LICENSE','LICENSE_SCOPE.md','THIRD_PARTY_NOTICES.md','SOURCE.txt','UcalManagerPackage.json') -or
            $relative -match '^(frontends|backends)/[^/]+/(frontend|backend)\.json$'){continue}
        throw "Unrecognized data payload in the user release: $relative"
    }
    $isFrontend=$relative -match '^frontends/[^/]+/(x64|x86)/UcalFrontend[^/]+\.dll$'
    $isCrt=$relative -match '^(runtime/(x64|x86)|frontends/[^/]+/(x64|x86))/(msvcp140(?:_1|_2|_atomic_wait|_codecvt_ids)?|vcruntime140(?:_1|_threads)?|concrt140|vccorlib140)\.dll$'
    if($relative -notin $expected -and -not $isFrontend -and -not $isCrt){throw "Unrecognized executable payload (vendor applications/runtimes are forbidden): $relative"}
    $pe=Get-UcalPeContract $file.FullName -ImportsOnly
    $expectedArchitecture='x64'
    if($relative -match '/(x64|x86)/'){$expectedArchitecture=$Matches[1]}
    if($pe.Machine -ne $expectedArchitecture){throw "Release runtime asset has the wrong architecture: $relative"}
    foreach($dependency in $pe.Imports){
        $name=[string]$dependency.Dll
        # API sets and Windows system libraries are OS prerequisites, never copied.
        if($name -match '^(api|ext)-ms-' -or ($name -notmatch '^(msvcp|vcruntime|concrt|vccorlib)' -and (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32/$name")))){continue}
        $candidates=@((Join-Path $file.DirectoryName $name),(Join-Path $root $name),
            (Join-Path $root "runtime/$($pe.Machine)/$name"))
        $match=@($candidates | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf} | Select-Object -First 1)
        if(-not $match.Count){throw "Missing release DLL dependency: $relative -> $name"}
        if((Get-UcalPeContract $match[0] -ImportsOnly).Machine -ne $pe.Machine){throw "Release DLL dependency has the wrong architecture: $relative -> $name"}
    }
}
foreach($category in @('frontends','backends')){
    foreach($metadata in Get-ChildItem -LiteralPath (Join-Path $root $category) -Filter '*.json' -Recurse -File){
        if($metadata.Name -notin @('frontend.json','backend.json')){continue}
        $m=Get-Content -LiteralPath $metadata.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if($m.PSObject.Properties['launch'] -and $m.launch.PSObject.Properties['plan']){throw 'User release metadata declares a developer script launcher.'}
        if($metadata.Name -eq 'backend.json' -and $m.configuration){throw 'Backend configuration must belong to Manager settings, not the release module.'}
    }
}
