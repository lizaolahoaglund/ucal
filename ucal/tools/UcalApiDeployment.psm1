function Invoke-UcalApiDeployment {
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Inspect','Apply','Restore')][string]$Action,
    [Parameter(Mandatory=$true)][string]$TargetExecutable,
    [Parameter(Mandatory=$true)][string]$ProfilePath,
    [hashtable]$SourcesByRole = @{},
    [string]$BackupDirectory,
    [ValidateRange(0,16)][int]$InjectFailureAfterFile = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'UcalPeContract.psm1') -Force

$targetExe = (Resolve-Path -LiteralPath $TargetExecutable).Path
if (-not (Test-Path -LiteralPath $targetExe -PathType Leaf)) {
    throw "Target executable not found: $TargetExecutable"
}
$targetDirectory = [IO.Path]::GetFullPath((Split-Path $targetExe -Parent)).TrimEnd('\')
$windowsRoot = [IO.Path]::GetFullPath([Environment]::GetFolderPath('Windows')).TrimEnd('\')
if ($targetDirectory -eq $windowsRoot -or
    $targetDirectory.StartsWith($windowsRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UCAL never performs application-local API deployment inside the Windows directory.'
}

$client = Get-UcalPeContract $targetExe
$profile = Get-Content -LiteralPath $ProfilePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($profile.schema -ne 'org.ucal.api-deployment-profile/1') { throw 'Unsupported API deployment profile.' }
if ($client.Machine -notin @('x86','x64')) { throw "Unsupported client architecture: $($client.Machine)" }
$contract = $profile.architectures.($client.Machine)
$contract | Add-Member -NotePropertyName ImportName -NotePropertyValue $contract.import_name
$contract | Add-Member -NotePropertyName Machine -NotePropertyValue $client.Machine
foreach ($file in @($contract.files)) {
    if ([IO.Path]::GetFileName([string]$file.name) -ne [string]$file.name -or $file.name -notmatch '^[A-Za-z0-9_.-]+\.dll$') {
        throw 'Invalid API deployment filename.'
    }
}
$matchingImports = @($client.Imports | Where-Object {
    [string]::Equals([string]$_.Dll, $contract.ImportName,
        [StringComparison]::OrdinalIgnoreCase)
})
if ($matchingImports.Count -ne 1) {
    throw "The target does not expose exactly one static $($contract.ImportName) import boundary. No files were changed."
}
$requiredSymbols = @($matchingImports[0].Symbols | Sort-Object -Unique)
if ($requiredSymbols.Count -eq 0 -or @($requiredSymbols | Where-Object { $_ -like '#*' }).Count -ne 0) {
    throw 'The API import boundary is empty or ordinal-only and cannot be verified by public function name.'
}
$normalizedSymbols = @($requiredSymbols | ForEach-Object { $_ -replace '^_', '' -replace '@\d+$', '' })
if (-not @($profile.entry_points | Where-Object { $_ -in $normalizedSymbols }).Count) {
    throw "The target API boundary does not contain a supported initialization entry point."
}

function Resolve-UcalDeploymentPath([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Label is required for $Action." }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { throw "$Label not found: $resolved" }
    return $resolved
}

function Resolve-UcalBackupPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "BackupDirectory is required for $Action." }
    $resolved = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ($resolved -eq $targetDirectory -or
        $resolved.StartsWith($targetDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The backup directory must remain outside the third-party application directory.'
    }
    return $resolved
}

if ($Action -eq 'Inspect') {
    [ordered]@{
        schema = [string]$profile.capability_schema
        status = 'compatible-static-import'
        targetExecutable = $targetExe
        machine = $client.Machine
        importedDll = $contract.ImportName
        importedSymbols = $requiredSymbols
        deploymentFiles = @($contract.files | ForEach-Object { [string]$_.name })
        executableMutation = $false
    } | ConvertTo-Json -Depth 5
    return
}


if ($Action -eq 'Apply') {
    $sources = @()
    foreach ($file in @($contract.files)) {
        $inputSource = $SourcesByRole[[string]$file.source_key]
        if ([string]::IsNullOrWhiteSpace($inputSource) -and $file.PSObject.Properties['source_adjacent_to']) {
            $anchorRole = [string]$file.source_adjacent_to
            $anchor = Resolve-UcalDeploymentPath $SourcesByRole[$anchorRole] $anchorRole
            $inputSource = Join-Path (Split-Path $anchor -Parent) ([string]$file.name)
        }
        $source = Resolve-UcalDeploymentPath $inputSource ([string]$file.source_key)
        $pe = Get-UcalPeContract $source
        if ($pe.Machine -ne $client.Machine) { throw "API file architecture must be $($client.Machine): $source" }
        $required = @($file.required_exports)
        if ($file.name -eq $contract.ImportName) { $required += $requiredSymbols }
        $missing = @($required | Where-Object { $_ -notin $pe.Exports })
        if ($missing.Count) { throw "API file is missing required functions: $($missing -join ', ')" }
        $sources += [pscustomobject]@{Source=$source; Target=(Join-Path $targetDirectory $file.name); Relative=[string]$file.name}
    }
    $backup = Resolve-UcalBackupPath $BackupDirectory
    if (Test-Path -LiteralPath $backup) { throw "Backup directory already exists: $backup" }
    New-Item -ItemType Directory -Path $backup | Out-Null
    $records = @()
    $mutated = @()
    try {
        for ($index = 0; $index -lt $sources.Count; $index++) {
            $entry = $sources[$index]
            $existed = Test-Path -LiteralPath $entry.Target -PathType Leaf
            $backupName = 'original-{0:D2}.bin' -f $index
            $originalHash = ''
            if ($existed) {
                Copy-Item -LiteralPath $entry.Target -Destination (Join-Path $backup $backupName)
                $originalHash = (Get-FileHash -LiteralPath $entry.Target -Algorithm SHA256).Hash
            }
            $record = [ordered]@{
                relativePath = $entry.Relative
                existed = $existed
                backupFile = if ($existed) { $backupName } else { '' }
                originalSha256 = $originalHash
                installedSha256 = (Get-FileHash -LiteralPath $entry.Source -Algorithm SHA256).Hash
            }
            $mutated += $record
            $stage = Join-Path $targetDirectory ('.ucal_api_stage_' + [guid]::NewGuid().ToString('N'))
            try {
                Copy-Item -LiteralPath $entry.Source -Destination $stage
                if ((Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash -ne
                    $record.installedSha256) { throw 'Staged API file hash verification failed.' }
                Move-Item -LiteralPath $stage -Destination $entry.Target -Force
            }
            finally {
                if (Test-Path -LiteralPath $stage -PathType Leaf) {
                    Remove-Item -LiteralPath $stage -Force
                }
            }
            if ((Get-FileHash -LiteralPath $entry.Target -Algorithm SHA256).Hash -ne
                $record.installedSha256) { throw 'Installed API file hash verification failed.' }
            $records += $record
            if ($InjectFailureAfterFile -ne 0 -and $records.Count -eq $InjectFailureAfterFile) {
                throw "Injected API deployment failure after file $InjectFailureAfterFile."
            }
        }
    $manifest = [ordered]@{
        schema = [string]$profile.backup_schema
        state = 'applied'
        targetExecutable = $targetExe
        targetExecutableSha256 = (Get-FileHash -LiteralPath $targetExe -Algorithm SHA256).Hash
        machine = $client.Machine
        importedDll = $contract.ImportName
        importedSymbols = $requiredSymbols
        executableMutation = $false
        entries = $records
    }
    [IO.File]::WriteAllText((Join-Path $backup 'deployment.json'),
        ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    }
    catch {
        foreach ($record in @($mutated)) {
            $target = Join-Path $targetDirectory ([string]$record.relativePath)
            if ([bool]$record.existed) {
                Copy-Item -LiteralPath (Join-Path $backup ([string]$record.backupFile)) `
                    -Destination $target -Force
            }
            elseif (Test-Path -LiteralPath $target -PathType Leaf) {
                Remove-Item -LiteralPath $target -Force
            }
        }
        throw
    }
    [ordered]@{
        status = 'applied'; targetExecutable = $targetExe; backupDirectory = $backup
        files = $records.Count; executableMutation = $false
    } | ConvertTo-Json -Depth 4
    return
}

$backup = Resolve-UcalBackupPath $BackupDirectory
$manifestPath = Join-Path $backup 'deployment.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "API deployment backup manifest not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$legacy = @()
if ($profile.PSObject.Properties['legacy_backups']) {
    $legacy = @($profile.legacy_backups | Where-Object { [string]$_.schema -eq [string]$manifest.schema })
}
if (([string]$manifest.schema -ne [string]$profile.backup_schema -and $legacy.Count -ne 1) -or
    -not [string]::Equals([string]$manifest.targetExecutable, $targetExe,
        [StringComparison]::OrdinalIgnoreCase) -or
    [string]$manifest.machine -ne $client.Machine -or
    [bool]$manifest.executableMutation) {
    throw 'API deployment backup does not match this exact executable contract.'
}
if ((Get-FileHash -LiteralPath $targetExe -Algorithm SHA256).Hash -ne
    [string]$manifest.targetExecutableSha256) {
    throw 'The target executable changed after Apply; refusing to alter its adjacent API files.'
}
$expectedNames = @()
if ([string]$manifest.schema -eq [string]$profile.backup_schema) {
    $expectedNames = @($contract.files | ForEach-Object { [string]$_.name })
} else { $expectedNames = @($legacy[0].architectures.($client.Machine)) }
$actualNames = @($manifest.entries | ForEach-Object { [string]$_.relativePath } | Sort-Object -Unique)
if (@($manifest.entries).Count -ne $expectedNames.Count -or
    (Compare-Object @($expectedNames | Sort-Object) @($actualNames | Sort-Object))) {
    throw 'API backup file list does not match the deployment profile.'
}
$restorePlan = @()
foreach ($record in @($manifest.entries)) {
    $relative = [string]$record.relativePath
    if ($relative -notin @($contract.files | ForEach-Object { [string]$_.name })) {
        throw "Unexpected API deployment manifest target: $relative"
    }
    $target = Join-Path $targetDirectory $relative
    if ([bool]$record.existed) {
        if ([string]$record.backupFile -notmatch '^original-[0-9]{2}\.bin$') { throw 'Invalid original backup filename.' }
        $original = Join-Path $backup ([string]$record.backupFile)
        if (-not (Test-Path -LiteralPath $original -PathType Leaf) -or
            (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne
                [string]$record.originalSha256) {
            throw "Original API backup is missing or changed: $original"
        }
    }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($hash -eq [string]$record.installedSha256) {
            $restorePlan += [pscustomobject]@{ Record = $record; Pending = $true }
        }
        elseif ([bool]$record.existed -and $hash -eq [string]$record.originalSha256) {
            $restorePlan += [pscustomobject]@{ Record = $record; Pending = $false }
        }
        else {
            throw "An applied application-local API file changed; refusing to overwrite it: $target"
        }
    }
    elseif (-not [bool]$record.existed) {
        $restorePlan += [pscustomobject]@{ Record = $record; Pending = $false }
    }
    else {
        throw "An original application-local API file is missing: $target"
    }
}
foreach ($item in $restorePlan) {
    if (-not $item.Pending) { continue }
    $record = $item.Record
    $target = Join-Path $targetDirectory ([string]$record.relativePath)
    if ([bool]$record.existed) {
        Copy-Item -LiteralPath (Join-Path $backup ([string]$record.backupFile)) `
            -Destination $target -Force
    }
    else {
        Remove-Item -LiteralPath $target -Force
    }
}
[IO.File]::WriteAllText((Join-Path $backup 'restored.json'),
    ([ordered]@{ restoredUtc = [DateTimeOffset]::UtcNow.ToString('o'); targetExecutable = $targetExe } |
        ConvertTo-Json), [Text.UTF8Encoding]::new($false))
[ordered]@{
    status = 'restored'; targetExecutable = $targetExe; backupDirectory = $backup
    files = @($manifest.entries).Count; executableMutation = $false
} | ConvertTo-Json -Depth 4

}
Export-ModuleMember -Function Invoke-UcalApiDeployment
