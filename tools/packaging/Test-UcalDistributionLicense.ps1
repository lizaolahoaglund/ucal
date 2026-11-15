[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageRoot, [string]$SourceArchivePath)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $PackageRoot).Path
$required=@('LICENSE','LICENSE_SCOPE.md','THIRD_PARTY_NOTICES.md',
    'licenses/GPL-3.0.txt','licenses/Independent-modules-exception.txt',
    'licenses/CompatibilitySupport.txt','licenses/ThirdPartyNotices.txt','SOURCE.txt')
foreach($relative in $required){
    if(-not(Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)){
        throw "Required distribution license/source file is missing: $relative"
    }
}
# Assembly verifies the collector's external SHA-256 receipt. After assembly,
# the normal package manifest protects this text just like every other asset.
$notices=Get-Content -LiteralPath (Join-Path $root 'licenses/ThirdPartyNotices.txt') -Raw -Encoding UTF8
foreach($title in @('UCAL third-party runtime notices','Qt 5.14.2','GCC runtime libraries',
    'MinGW-w64 runtime','winpthreads runtime','Microsoft runtime components')){
    if($notices -notmatch ('(?m)^'+[regex]::Escape($title)+'\r?$')){
        throw "Required runtime notice section is missing: $title"
    }
}
$source=Get-Content -LiteralPath (Join-Path $root 'SOURCE.txt') -Raw -Encoding UTF8
if($source -match '(?m)^Edition:' -and $source -notmatch '(?m)^Edition: (community|internal)\r?$'){
    throw 'Invalid package edition.'
}
if($source -notmatch '(?m)^Revision: ([0-9a-f]{40})\r?$'){throw 'Invalid corresponding-source revision.'}
$revision=$Matches[1]
if($source -notmatch '(?m)^Archive: (UcalManager-source-[0-9a-f]{12}\.tar)\r?$' -or
    $Matches[1] -ne ('UcalManager-source-'+$revision.Substring(0,12)+'.tar')){throw 'Invalid corresponding-source archive name.'}
if($source -notmatch '(?m)^SHA256: ([0-9A-Fa-f]{64})\r?$'){throw 'Invalid corresponding-source hash.'}
$sourceHash=$Matches[1]
if($source -notmatch '(?m)^License: LGPL-3.0-only WITH Independent-modules-exception\r?$'){
    throw 'Invalid corresponding-source license.'
}
if($SourceArchivePath -and ((Get-FileHash -LiteralPath $SourceArchivePath -Algorithm SHA256).Hash -ne $sourceHash)){
    throw 'Corresponding-source archive integrity mismatch.'
}
