param(
    [Parameter(Mandatory = $true)]
    [string]$DumpbinPath,
    [Parameter(Mandatory = $true)]
    [string]$InputDll,
    [Parameter(Mandatory = $true)]
    [string]$OutputDef,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')]
    [string]$ForwardModule,
    [ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')]
    [string]$LibraryName = 'vcndrvms64',
    [ValidateRange(1, 65535)]
    [int]$MinimumNamedExports = 100,
    [ValidatePattern('^$|^[_A-Za-z][_A-Za-z0-9@]*(,[_A-Za-z][_A-Za-z0-9@]*)*$')]
    [string]$OverrideName = '',
    [ValidatePattern('^$|^[A-Za-z_][A-Za-z0-9_]*$')]
    [string]$OverrideModule = '',
    [ValidatePattern('^$|^[_A-Za-z][_A-Za-z0-9@]*(,[_A-Za-z][_A-Za-z0-9@]*)*$')]
    [string]$RequiredExportNames = '',
    [string]$ExpectedSha256 = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DumpbinPath -PathType Leaf)) {
    throw "dumpbin not found: $DumpbinPath"
}
if (-not (Test-Path -LiteralPath $InputDll -PathType Leaf)) {
    throw "input DLL not found: $InputDll"
}
if ($ExpectedSha256) {
    $actualHash = (Get-FileHash -LiteralPath $InputDll -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedSha256) {
        throw "input DLL hash mismatch: expected $ExpectedSha256, actual $actualHash"
    }
}
if (($OverrideName -eq '') -ne ($OverrideModule -eq '')) {
    throw 'OverrideName and OverrideModule must be provided together'
}
$overrideNames = if ($OverrideName) { @($OverrideName -split ',') } else { @() }

$exports = @()
$reportedFunctions = $null
$reportedNames = $null
$ordinalBase = $null
$dumpbinOutput = & $DumpbinPath /nologo /exports $InputDll
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed with exit code $LASTEXITCODE"
}
foreach ($line in $dumpbinOutput) {
    if ($line -match '^\s*(\d+) ordinal base\s*$') {
        $ordinalBase = [int]$matches[1]
    } elseif ($line -match '^\s*(\d+) number of functions\s*$') {
        $reportedFunctions = [int]$matches[1]
    } elseif ($line -match '^\s*(\d+) number of names\s*$') {
        $reportedNames = [int]$matches[1]
    } elseif ($line -match '^\s*(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)(?:\s+=\s+.+)?$') {
        $exports += [pscustomobject]@{
            Ordinal = [int]$matches[1]
            Name = $matches[2]
        }
    }
}

if ($exports.Count -lt $MinimumNamedExports) {
    throw "refusing to generate an incomplete proxy: only $($exports.Count) named exports found"
}
if ($reportedNames -ne $exports.Count -or $null -eq $ordinalBase -or $reportedFunctions -lt $reportedNames) {
    throw "refusing an unparsed export table: base=$ordinalBase functions=$reportedFunctions, names=$reportedNames, parsed=$($exports.Count)"
}
if (@($exports | Group-Object Name | Where-Object Count -gt 1).Count -ne 0) {
    throw 'duplicate export names found'
}
if (@($exports | Group-Object Ordinal | Where-Object Count -gt 1).Count -ne 0) {
    throw 'duplicate export ordinals found'
}
foreach ($export in $exports) {
    if ($export.Name -notmatch '^(?:[_A-Za-z][_A-Za-z0-9@]*|\?[_A-Za-z0-9?@$]+)$') {
        throw "unsupported decorated export name: $($export.Name)"
    }
}
$requiredExports = if ($RequiredExportNames) { @($RequiredExportNames -split ',') } else { @() }
$availableExportNames = @($exports | Select-Object -ExpandProperty Name)
$missingRequiredExports = @($requiredExports | Where-Object { $_ -notin $availableExportNames })
if ($missingRequiredExports.Count -ne 0) {
    throw "input DLL is missing required exports: $($missingRequiredExports -join ', ')"
}

$outputDirectory = Split-Path -Parent $OutputDef
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("LIBRARY $LibraryName")
$lines.Add('EXPORTS')
foreach ($export in $exports | Sort-Object Ordinal) {
    $module = if ($overrideNames -contains $export.Name) { $OverrideModule } else { $ForwardModule }
    if ($export.Name.StartsWith('?')) {
        # Preserve vendor C++ names as opaque PE exports. Forward by the
        # original ordinal; UCAL neither declares nor implements that C++ ABI.
        $lines.Add(('    {0}={1}.#{2} @{2}' -f $export.Name,$module,$export.Ordinal))
    } else {
        $lines.Add("    $($export.Name)=$module.$($export.Name) @$($export.Ordinal)")
    }
}
$namedOrdinals = @{}
foreach ($export in $exports) { $namedOrdinals[$export.Ordinal] = $true }
for ($ordinal = $ordinalBase; $ordinal -lt $ordinalBase + $reportedFunctions; ++$ordinal) {
    if (-not $namedOrdinals.ContainsKey($ordinal)) {
        $lines.Add("    UcalOrdinal$ordinal=$ForwardModule.#$ordinal @$ordinal NONAME")
    }
}
[IO.File]::WriteAllLines($OutputDef, $lines, [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    status = 'generated'
    input = (Resolve-Path -LiteralPath $InputDll).Path
    output = $OutputDef
    forwardModule = $ForwardModule
    overrideName = $OverrideName
    overrideModule = $OverrideModule
    namedExports = $exports.Count
    ordinalSlots = $reportedFunctions
    requiredExports = $requiredExports
} | ConvertTo-Json -Compress
