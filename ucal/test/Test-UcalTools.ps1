[CmdletBinding()]
param(
    [string]$UcalRoot,
    [string]$OutputDirectory,
    [string]$FixtureDirectory,
    [string]$VcVarsAllPath = $env:UCAL_VCVARSALL
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../../tools/UcalToolchain.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '../tools/UcalPeContract.psm1') -Force
if (-not $UcalRoot) { $UcalRoot = Join-Path $PSScriptRoot '..' }
$ucalRoot = (Resolve-Path -LiteralPath $UcalRoot).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot ('../../out/ucal-tools-' + [Guid]::NewGuid().ToString('N')) }
if (-not $FixtureDirectory) { $FixtureDirectory = Join-Path $PSScriptRoot '.' }
$fixtureRoot = (Resolve-Path -LiteralPath $FixtureDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Use a new or empty output directory.' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
$cases = @()
$saved_environment = @{}
Get-ChildItem Env: | ForEach-Object { $saved_environment[$_.Name] = $_.Value }
try {
    foreach ($architecture in @('x64','x86')) {
        $null = Import-UcalMsvcEnvironment $architecture $VcVarsAllPath
        $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
        $dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
        $directory = Join-Path $output $architecture
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $fixture = Join-Path $directory 'fixture.dll'
        $null = Invoke-UcalLoggedProcess $compiler @('/nologo','/LD','/MT',
            (Join-Path $fixtureRoot 'ForwarderMock.c'), "/Fe:$fixture", '/link',
            ('/DEF:' + (Join-Path $fixtureRoot 'ForwarderMock.def'))) $directory (Join-Path $directory 'build.log')
        $definition = Join-Path $directory 'forwarder.def'
        $null = & (Join-Path $ucalRoot 'tools/generate-forwarder-def.ps1') -DumpbinPath $dumpbin `
            -InputDll $fixture -OutputDef $definition -ForwardModule fixture_origin `
            -LibraryName fixture_proxy -MinimumNamedExports 2 -RequiredExportNames 'UcalFixtureAlpha,UcalFixtureBeta'
        $text = [IO.File]::ReadAllText($definition)
        foreach ($expected in @('UcalFixtureAlpha=fixture_origin.UcalFixtureAlpha @1',
                                'UcalFixtureBeta=fixture_origin.UcalFixtureBeta @3',
                                'UcalOrdinal5=fixture_origin.#5 @5 NONAME')) {
            if (-not $text.Contains($expected)) { throw "Missing export mapping: $expected" }
        }
        $cases += @{id='named-and-ordinal-exports';architecture=$architecture;status='passed'}
        $completePe=Get-UcalPeContract $fixture
        $importsPe=Get-UcalPeContract $fixture -ImportsOnly
        if($importsPe.Machine -ne $architecture -or @($importsPe.Exports).Count -ne 0 -or
            ($importsPe.Imports | ConvertTo-Json -Depth 8 -Compress) -ne ($completePe.Imports | ConvertTo-Json -Depth 8 -Compress)){
            throw 'Imports-only PE inspection changed architecture or dependency results.'
        }
        $rejected = $false
        try {
            $null = & (Join-Path $ucalRoot 'tools/generate-forwarder-def.ps1') -DumpbinPath $dumpbin `
                -InputDll $fixture -OutputDef (Join-Path $directory 'invalid.def') -ForwardModule fixture_origin `
                -MinimumNamedExports 2 -RequiredExportNames absentExport
        } catch {
            if ($_.Exception.Message -notmatch 'missing required exports') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw 'Missing required export was accepted.' }
        $cases += @{id='missing-contract-rejected';architecture=$architecture;status='passed'}
        $origin=Join-Path $directory 'fixture_decorated_origin.dll'
        $null=Invoke-UcalLoggedProcess $compiler @('/nologo','/LD','/MT',
            (Join-Path $fixtureRoot 'ForwarderMock.c'),(Join-Path $fixtureRoot 'ForwarderDecoratedMock.cpp'),
            "/Fe:$origin",'/link',('/DEF:'+(Join-Path $fixtureRoot 'ForwarderMock.def'))) $directory (Join-Path $directory 'decorated-build.log')
        $proxyDef=Join-Path $directory 'decorated-proxy.def'
        $null=& (Join-Path $ucalRoot 'tools/generate-forwarder-def.ps1') -DumpbinPath $dumpbin `
            -InputDll $origin -OutputDef $proxyDef -ForwardModule fixture_decorated_origin `
            -LibraryName fixture_decorated_proxy -MinimumNamedExports 3
        $proxy=Join-Path $directory 'fixture_decorated_proxy.dll'
        $linker=(Get-Command link.exe -ErrorAction Stop).Source
        $null=Invoke-UcalLoggedProcess $linker @('/NOLOGO','/DLL','/NOENTRY',('/DEF:'+$proxyDef),
            ('/OUT:'+$proxy),('/MACHINE:'+$architecture)) $directory (Join-Path $directory 'decorated-link.log')
        $probe=Join-Path $directory 'ForwarderProbe.exe'
        $null=Invoke-UcalLoggedProcess $compiler @('/nologo','/MT',(Join-Path $fixtureRoot 'ForwarderProbe.c'),('/Fe:'+$probe)) $directory (Join-Path $directory 'probe-build.log')
        $null=Invoke-UcalLoggedProcess $probe @($proxy) $directory (Join-Path $directory 'decorated-call.log')
        $cases+=@{id='opaque-decorated-export-call';architecture=$architecture;status='passed'}
        $malformed=Join-Path $directory 'malformed.dll'
        $malformedBytes=[IO.File]::ReadAllBytes($origin)
        $marker=[Text.Encoding]::ASCII.GetBytes('?opaqueVendorExport@@YAHXZ')
        $markerOffset=-1
        for($offset=0;$offset -le $malformedBytes.Length-$marker.Length;$offset++){
            $matchesMarker=$true
            for($index=0;$index -lt $marker.Length;$index++){
                if($malformedBytes[$offset+$index] -ne $marker[$index]){$matchesMarker=$false;break}
            }
            if($matchesMarker){$markerOffset=$offset;break}
        }
        if($markerOffset -lt 0){throw 'Decorated fixture export name not found.'}
        $malformedBytes[$markerOffset+7]=[byte][char]';'
        [IO.File]::WriteAllBytes($malformed,$malformedBytes)
        $rejected=$false
        try{
            $null=& (Join-Path $ucalRoot 'tools/generate-forwarder-def.ps1') -DumpbinPath $dumpbin `
                -InputDll $malformed -OutputDef (Join-Path $directory 'malformed-proxy.def') `
                -ForwardModule fixture_decorated_origin -MinimumNamedExports 3
        }catch{
            if($_.Exception.Message -notmatch 'unsupported decorated export name'){throw}
            $rejected=$true
        }
        if(-not $rejected){throw 'Unsafe export-name characters were accepted.'}
        $cases+=@{id='unsafe-export-name-rejected';architecture=$architecture;status='passed'}
    }
} finally {
    Get-ChildItem Env: | Where-Object { -not $saved_environment.ContainsKey($_.Name) } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
    foreach ($name in $saved_environment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved_environment[$name], 'Process')
    }
    $status = if ($cases.Count -eq 8) { 'passed' } else { 'failed' }
    [IO.File]::WriteAllText((Join-Path $output 'summary.json'), ([ordered]@{
        schema='org.ucal.tools-test/1';status=$status;hardwareAccess=$false;cases=$cases
    } | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}
