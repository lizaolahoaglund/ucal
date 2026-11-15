function Read-UcalQtResults([string]$Path) {
    $document = [Xml.XmlDocument]::new()
    try { $document.Load($Path) } catch { throw "Incomplete or invalid Qt XML report: $Path" }
    if ($document.DocumentElement.LocalName -ne 'TestCase') { throw 'Expected a QtTest TestCase report.' }
    foreach ($item in $document.SelectNodes('/TestCase/TestFunction')) {
        $fail = @($item.SelectNodes('Incident[@type="fail" or @type="xpass" or @type="xfail" or @type="fatal"] | Message[@type="qfatal"]'))
        $skip = @($item.SelectNodes('Message[@type="skip"] | Incident[@type="skip"]'))
        $pass = @($item.SelectNodes('Incident[@type="pass"]'))
        $status = if ($fail.Count) { 'failed' } elseif ($skip.Count) { 'skipped' } elseif ($pass.Count) { 'passed' } else { 'failed' }
        $diagnostics = @($item.SelectNodes('Incident[@type!="pass"]/Description | Message/Description') | ForEach-Object { $_.InnerText })
        $duration = $item.SelectSingleNode('Duration')
        $seconds = if ($duration) { [double]::Parse($duration.GetAttribute('msecs'), [Globalization.CultureInfo]::InvariantCulture) / 1000 } else { 0 }
        [pscustomobject]@{ id = $item.GetAttribute('name'); status = $status; seconds = $seconds; diagnostics = $diagnostics }
    }
}

function Write-UcalQtJUnit([object[]]$Records, [string]$Path) {
    $document = [Xml.XmlDocument]::new()
    $null = $document.AppendChild($document.CreateXmlDeclaration('1.0', 'utf-8', $null))
    $suite = $document.CreateElement('testsuite')
    $null = $document.AppendChild($suite)
    $suite.SetAttribute('name', 'ManagerModelTest')
    $suite.SetAttribute('tests', [string]$Records.Count)
    $suite.SetAttribute('failures', [string]@($Records | Where-Object { $_.status -eq 'failed' }).Count)
    $suite.SetAttribute('skipped', [string]@($Records | Where-Object { $_.status -eq 'skipped' }).Count)
    $suite.SetAttribute('errors', '0')
    foreach ($record in $Records) {
        $test = $document.CreateElement('testcase')
        $test.SetAttribute('classname', 'ManagerModelTest')
        $test.SetAttribute('name', $record.id)
        $test.SetAttribute('time', $record.seconds.ToString('0.######', [Globalization.CultureInfo]::InvariantCulture))
        $null = $suite.AppendChild($test)
        if ($record.status -ne 'passed') {
            $element = $document.CreateElement($(if ($record.status -eq 'skipped') { 'skipped' } else { 'failure' }))
            $element.InnerText = $record.diagnostics -join [Environment]::NewLine
            $null = $test.AppendChild($element)
        } elseif ($record.diagnostics.Count) {
            # Qt 5.14's xunit logger counts ordinary qwarn messages as errors.
            # Preserve those messages without turning a passing test into a failure.
            $element = $document.CreateElement('system-out')
            $element.InnerText = $record.diagnostics -join [Environment]::NewLine
            $null = $test.AppendChild($element)
        }
    }
    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Encoding = [Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $writer = [Xml.XmlWriter]::Create($Path, $settings)
    try { $document.Save($writer) } finally { $writer.Dispose() }
}
Export-ModuleMember -Function Read-UcalQtResults, Write-UcalQtJUnit
