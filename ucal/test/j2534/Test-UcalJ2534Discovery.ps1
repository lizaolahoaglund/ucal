[CmdletBinding()]
param([ValidateSet('x64','Win32')][string[]]$Architecture=@('x64','Win32'),[string]$EvidenceDirectory)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $EvidenceDirectory){$EvidenceDirectory=Join-Path $root 'out\j2534-public-discovery'}
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidence=(Resolve-Path $EvidenceDirectory).Path
$savedConfig=$env:UCAL_J2534_CONFIG; $savedMode=$env:UCAL_DISCOVERY_TEST_MODE
$results=@()
try {
    foreach($arch in $Architecture){
        $bin=Join-Path $root "out\$arch\Release"
        $descriptions = @(Get-ChildItem -LiteralPath (Join-Path $root 'backends') -Filter discovery.json -Recurse -File | Where-Object { $_.Directory.Name -eq 'test' })
        foreach($description in $descriptions){
            $provider = Get-Content -LiteralPath $description.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            $family = [string]$provider.id
            $directory=Join-Path $evidence "$arch-$family"
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
            $nativePath=Join-Path $directory $provider.native_names.$arch
            Copy-Item -LiteralPath (Join-Path $bin $provider.mock) -Destination $nativePath -Force
            $helper=Join-Path $bin $provider.helper
            $config=Join-Path $directory 'provider.ini'
            @"
[provider]
runtime=$(Join-Path $directory 'vendor.dll')
discovery_library=$helper
test_native=$nativePath
"@ | Set-Content -LiteralPath $config -Encoding Unicode
            $env:UCAL_J2534_CONFIG=$config
            $modes=@($provider.modes)
            foreach($mode in $modes){
                $env:UCAL_DISCOVERY_TEST_MODE=$mode
                $case="$($provider.scenario)-$mode"; $stdout=Join-Path $directory "$case.stdout.jsonl"; $stderr=Join-Path $directory "$case.stderr.log"
                $process=Start-Process -FilePath (Join-Path $bin 'UcalJ2534Harness.exe') -ArgumentList $case -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
                $null=$process.Handle
                if(-not $process.WaitForExit(15000)){Stop-Process -Id $process.Id;throw "Discovery timeout: $arch/$case"}
                $process.WaitForExit()
                $results+=[pscustomobject]@{architecture=$arch;case=$case;exitCode=$process.ExitCode;output=$stdout}
                if($process.ExitCode -ne 0){throw "Discovery failed: $arch/$case; see $stderr"}
            }
        }
    }
    [ordered]@{status='passed';hardware=$false;cases=$results} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'result.json') -Encoding UTF8
    "Public J2534 discovery cases passed: $($results.Count)"
}finally{$env:UCAL_J2534_CONFIG=$savedConfig;$env:UCAL_DISCOVERY_TEST_MODE=$savedMode}
