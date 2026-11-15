[CmdletBinding()]
param(
    [string]$Root,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path }
$runtime = Join-Path $Root "out/x64/$Configuration"
$output = Join-Path $Root ('out/test-logs/pcanview-support-' + [Guid]::NewGuid().ToString('N'))
$bridge = Join-Path $runtime 'UcalPcanViewNativeBridge.exe'
$fixture = Join-Path $runtime 'UcalPcanViewSupportFixture.dll'
$invalid = Join-Path $runtime 'UcalPcanBasicTestRuntime.dll'
$results = @()
function Run-Case([string]$Name, [string]$Library, [string[]]$Arguments, [int]$Expected) {
    $directory = Join-Path $output $Name
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $executable = Join-Path $directory 'UcalPcanViewNativeBridge.exe'
    Copy-Item -LiteralPath $bridge -Destination $executable
    if ($Library) { Copy-Item -LiteralPath $Library -Destination (Join-Path $directory 'UcalPcanViewSupport.dll') }
    $savedErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $text = & $executable @Arguments 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedErrorPreference }
    [IO.File]::WriteAllText((Join-Path $directory 'result.txt'), $text, [Text.UTF8Encoding]::new($false))
    if ($code -ne $Expected) { throw "$Name expected $Expected, received $code. $text" }
    $script:results += [ordered]@{id=$Name;status='passed';exitCode=$code}
}
Run-Case 'utf16 arguments with spaces' $fixture @(([string][char]0x53c2 + [char]0x6570 + ' ' + [char]0x901a + [char]0x9053), '--fixture') 0
Run-Case 'support-exit-code-17' $fixture @('--exit-17') 17
Run-Case 'missing-library' '' @() 3
Run-Case 'incompatible-export' $invalid @() 3
$engine = Join-Path $runtime 'UcalPcanViewSupport.dll'
$privateEngine = 'not-supplied'
if (Test-Path -LiteralPath $engine -PathType Leaf) {
    Run-Case 'private-engine-invalid-input' $engine @() 2
    $privateEngine = 'passed: invalid input, no vendor launch'
}
$summary = [ordered]@{schema='org.ucal.pcanview-support-contract/1';status='passed';
    architecture='x64';cases=$results;privateEngine=$privateEngine;hardwareAccess=$false}
[IO.File]::WriteAllText((Join-Path $output 'summary.json'), ($summary | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
$summary | ConvertTo-Json -Depth 5
# Expected missing/incompatible-library cases return nonzero child statuses.
# Normalize only after every assertion and the complete report have succeeded.
$global:LASTEXITCODE = 0
