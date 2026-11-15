[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'UcalQtResults.psm1') -Force
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '../..')) ('out/regression/report-' + [Guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$path = Join-Path $OutputDirectory 'fixture.xml'
[IO.File]::WriteAllText($path, @'
<?xml version="1.0" encoding="UTF-8"?>
<TestCase name="ManagerModelTest">
 <TestFunction name="pass"><Incident type="pass"/><Duration msecs="1.5"/></TestFunction>
 <TestFunction name="skip"><Message type="skip"><Description>&#x5171; optional platform</Description></Message></TestFunction>
 <TestFunction name="fail"><Incident type="fail"><Description>assertion &lt;failed&gt;</Description></Incident></TestFunction>
 <TestFunction name="warning"><Message type="qwarn"><Description>platform warning</Description></Message><Incident type="pass"/></TestFunction>
 <TestFunction name="missing"/>
 <TestFunction name="fatal"><Message type="qfatal"><Description>fatal</Description></Message><Incident type="pass"/></TestFunction>
</TestCase>
'@, [Text.UTF8Encoding]::new($false))
$records = @(Read-UcalQtResults $path)
function Assert([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
Assert ($records.Count -eq 6) 'Each function must have exactly one result.'
Assert ($records[0].status -eq 'passed' -and $records[0].seconds -eq 0.0015) 'Pass and duration conversion failed.'
Assert ($records[1].status -eq 'skipped' -and $records[1].diagnostics[0].StartsWith([string][char]0x5171)) 'Qt Message skip or UTF-8 decoding failed.'
Assert ($records[2].status -eq 'failed') 'Failed assertion became a pass.'
Assert ($records[3].status -eq 'passed' -and $records[3].diagnostics.Count -eq 1) 'Warnings must be retained without fabricating failure.'
Assert ($records[4].status -eq 'failed' -and $records[5].status -eq 'failed') 'Missing or fatal results must fail closed.'
$junit = Join-Path $OutputDirectory 'fixture-junit.xml'
Write-UcalQtJUnit $records $junit
$document = [Xml.XmlDocument]::new(); $document.Load($junit)
Assert ($document.testsuite.tests -eq '6' -and $document.testsuite.failures -eq '3' -and $document.testsuite.skipped -eq '1' -and $document.testsuite.errors -eq '0') 'JUnit result counts differ.'
Assert ($document.SelectNodes('/testsuite/testcase[@name="warning"]/system-out').Count -eq 1) 'JUnit lost the warning output.'
$bad = Join-Path $OutputDirectory 'truncated.xml'; [IO.File]::WriteAllText($bad, '<TestCase>')
$rejected = $false
try { $null = Read-UcalQtResults $bad } catch { $rejected = $true }
Assert $rejected 'Truncated report was accepted.'
[ordered]@{ schema = 'org.ucal.qt-report-contract/1'; status = 'passed'; assertions = 9 } | ConvertTo-Json
