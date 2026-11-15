[CmdletBinding()]
param(
    [ValidateSet('Vxl','PcanBasic')][string]$Api='Vxl',
    [string]$DeploymentScript,
    [string]$FrontendX64,
    [string]$DiscoveryX64,
    [string]$FrontendX86,
    [string]$DiscoveryX86
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$folder=if($Api -eq 'Vxl'){'vxlapi'}else{'pcan_basic'}
$profile=Get-Content -LiteralPath (Join-Path $repoRoot "frontends/$folder/deployment.json") -Raw | ConvertFrom-Json
$dll64=[string]$profile.architectures.x64.import_name
$dll32=[string]$profile.architectures.x86.import_name
$entry=[string]$profile.entry_points[0]
$sidecar64=if($Api -eq 'Vxl'){'UcalXlDiscovery64.dll'}else{'unrelated.dat'}
$sidecar32=if($Api -eq 'Vxl'){'UcalXlDiscovery.dll'}else{'unrelated.dat'}
if(-not $DeploymentScript){$DeploymentScript="frontends/$folder/tools/Set-Ucal$($Api)ClientDeployment.ps1"}
if(-not $FrontendX64){$FrontendX64="out/x64/Release/$dll64"}
if(-not $FrontendX86){$FrontendX86="out/Win32/Release/$dll32"}
if(-not $DiscoveryX64){$DiscoveryX64='out/x64/Release/UcalXlDiscovery64.dll'}
if(-not $DiscoveryX86){$DiscoveryX86='out/Win32/Release/UcalXlDiscovery.dll'}
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out')).TrimEnd('\')
$testRoot = [IO.Path]::GetFullPath((Join-Path $outRoot (
    'api-client-deployment-acceptance\' + [guid]::NewGuid().ToString('N')))).TrimEnd('\')
if (-not $testRoot.StartsWith($outRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'API deployment fixture escaped the ignored out directory.'
}
$script = (Resolve-Path (Join-Path $repoRoot $DeploymentScript)).Path
$frontend64 = (Resolve-Path (Join-Path $repoRoot $FrontendX64)).Path
$discovery64 = if($Api -eq 'Vxl'){(Resolve-Path (Join-Path $repoRoot $DiscoveryX64)).Path}else{$frontend64}
$frontend32 = (Resolve-Path (Join-Path $repoRoot $FrontendX86)).Path
$discovery32 = if($Api -eq 'Vxl'){(Resolve-Path (Join-Path $repoRoot $DiscoveryX86)).Path}else{$frontend32}
$powerShell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Set-UcalUInt16([byte[]]$Bytes, [int]$Offset, [uint16]$Value) {
    [Buffer]::BlockCopy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 2)
}

function Set-UcalUInt32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    [Buffer]::BlockCopy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 4)
}

function Set-UcalUInt64([byte[]]$Bytes, [int]$Offset, [uint64]$Value) {
    [Buffer]::BlockCopy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 8)
}

function Set-UcalAscii([byte[]]$Bytes, [int]$Offset, [string]$Value) {
    $encoded = [Text.Encoding]::ASCII.GetBytes($Value + [char]0)
    [Buffer]::BlockCopy($encoded, 0, $Bytes, $Offset, $encoded.Length)
}

function New-UcalImportFixture {
    param(
        [string]$Path,
        [ValidateSet('x86', 'x64')][string]$Machine = 'x64',
        [string]$DllName = '',
        [string[]]$Symbols = @(),
        [switch]$NoImports
    )
    if (-not $Symbols.Count) { $Symbols = @($entry) }
    if ($Symbols.Count -gt 3) { throw 'Synthetic fixture supports at most three import symbols.' }
    if ([string]::IsNullOrWhiteSpace($DllName)) {
        $DllName = if ($Machine -eq 'x64') { $dll64 } else { $dll32 }
    }
    $bytes = [byte[]]::new(0x800)
    Set-UcalUInt16 $bytes 0 0x5A4D
    Set-UcalUInt32 $bytes 0x3C 0x80
    Set-UcalUInt32 $bytes 0x80 0x00004550
    Set-UcalUInt16 $bytes 0x84 $(if ($Machine -eq 'x64') { 0x8664 } else { 0x014C })
    Set-UcalUInt16 $bytes 0x86 1
    $optionalSize = if ($Machine -eq 'x64') { 0xF0 } else { 0xE0 }
    $optionalMagic = if ($Machine -eq 'x64') { 0x20B } else { 0x10B }
    $directoryCountOffset = if ($Machine -eq 'x64') { 108 } else { 92 }
    $directoryOffset = if ($Machine -eq 'x64') { 112 } else { 96 }
    $sectionOffset = 0x98 + $optionalSize
    Set-UcalUInt16 $bytes 0x94 $optionalSize
    Set-UcalUInt16 $bytes 0x98 $optionalMagic
    Set-UcalUInt32 $bytes (0x98 + 60) 0x200
    Set-UcalUInt32 $bytes (0x98 + $directoryCountOffset) 16
    if (-not $NoImports) {
        Set-UcalUInt32 $bytes (0x98 + $directoryOffset + 8) 0x1000
        Set-UcalUInt32 $bytes (0x98 + $directoryOffset + 12) 40
    }
    Set-UcalAscii $bytes $sectionOffset '.rdata'
    Set-UcalUInt32 $bytes ($sectionOffset + 8) 0x600
    Set-UcalUInt32 $bytes ($sectionOffset + 12) 0x1000
    Set-UcalUInt32 $bytes ($sectionOffset + 16) 0x600
    Set-UcalUInt32 $bytes ($sectionOffset + 20) 0x200
    if (-not $NoImports) {
        Set-UcalUInt32 $bytes 0x200 0x1040
        Set-UcalUInt32 $bytes (0x200 + 12) 0x1080
        Set-UcalUInt32 $bytes (0x200 + 16) 0x1060
        Set-UcalAscii $bytes 0x280 $DllName
        for ($index = 0; $index -lt $Symbols.Count; $index++) {
            $nameRva = [uint64](0x10A0 + ($index * 0x40))
            if ($Machine -eq 'x64') {
                Set-UcalUInt64 $bytes (0x240 + ($index * 8)) $nameRva
                Set-UcalUInt64 $bytes (0x260 + ($index * 8)) $nameRva
            }
            else {
                Set-UcalUInt32 $bytes (0x240 + ($index * 4)) ([uint32]$nameRva)
                Set-UcalUInt32 $bytes (0x260 + ($index * 4)) ([uint32]$nameRva)
            }
            Set-UcalUInt16 $bytes (0x2A0 + ($index * 0x40)) 0
            Set-UcalAscii $bytes (0x2A2 + ($index * 0x40)) $Symbols[$index]
        }
    }
    $parent = Split-Path $Path -Parent
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-UcalUtf8([string]$Path, [string]$Text) {
    $parent = Split-Path $Path -Parent
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Get-UcalFingerprint([string]$Root) {
    return @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($Root.TrimEnd('\').Length + 1).Replace('\', '/')
        '{0}|{1}' -f $relative, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    } | Sort-Object)
}

function Invoke-UcalDeployment([string[]]$Arguments) {
    if($Api -eq 'PcanBasic'){
        $filtered=@()
        for($i=0;$i -lt $Arguments.Count;$i++){
            if($Arguments[$i] -eq '-DiscoveryDll'){$i++;continue}
            $filtered+=$Arguments[$i]
        }
        $Arguments=$filtered
    }
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $powerShell -NoProfile -ExecutionPolicy Bypass -File $script @Arguments 2>&1 |
            ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
        return [pscustomobject]@{ ExitCode = $exitCode; Text = ($lines -join [Environment]::NewLine) }
    }
    finally { $ErrorActionPreference = $saved }
}

function Assert-Ucal([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$clientRoot = Join-Path $testRoot 'client'
$clientExe = Join-Path $clientRoot 'sample-client.exe'
$backup = Join-Path $testRoot 'backup'
$faultBackup = Join-Path $testRoot 'backup-fault'

try {
    New-UcalImportFixture $clientExe
    Write-UcalUtf8 (Join-Path $clientRoot $dll64) 'original application-local API'
    Write-UcalUtf8 (Join-Path $clientRoot $sidecar64) 'original adjacent sidecar name'
    $baseline = @(Get-UcalFingerprint $clientRoot)
    $originalExeBytes = [IO.File]::ReadAllBytes($clientExe)
    $originalExeHash = (Get-FileHash $clientExe -Algorithm SHA256).Hash

    $inspect = Invoke-UcalDeployment @('-Action', 'Inspect', '-TargetExecutable', $clientExe)
    Assert-Ucal ($inspect.ExitCode -eq 0) "API capability inspection failed: $($inspect.Text)"
    $inspection = $inspect.Text | ConvertFrom-Json
    Assert-Ucal ([string]$inspection.status -eq 'compatible-static-import') `
        'Static API capability was not reported.'
    Assert-Ucal (-not [bool]$inspection.executableMutation) 'Inspection allowed EXE mutation.'

    $apply = Invoke-UcalDeployment @('-Action', 'Apply', '-TargetExecutable', $clientExe,
        '-FrontendDll', $frontend64, '-DiscoveryDll', $discovery64,
        '-BackupDirectory', $backup)
    Assert-Ucal ($apply.ExitCode -eq 0) "API Apply failed: $($apply.Text)"
    Assert-Ucal ((Get-FileHash (Join-Path $clientRoot $dll64) -Algorithm SHA256).Hash -eq
        (Get-FileHash $frontend64 -Algorithm SHA256).Hash) 'Frontend deployment hash changed.'
    if($Api -eq 'Vxl'){
        Assert-Ucal ((Get-FileHash (Join-Path $clientRoot $sidecar64) -Algorithm SHA256).Hash -eq
            (Get-FileHash $discovery64 -Algorithm SHA256).Hash) 'Discovery deployment hash changed.'
    }else{
        Assert-Ucal ((Get-Content -LiteralPath (Join-Path $clientRoot $sidecar64) -Raw) -eq
            'original adjacent sidecar name') 'PCAN deployment changed an unrelated application file.'
    }
    Assert-Ucal ((Get-FileHash $clientExe -Algorithm SHA256).Hash -eq $originalExeHash) `
        'Apply modified the target executable.'

    $changedExeBytes = [byte[]]::new($originalExeBytes.Length + 1)
    [Buffer]::BlockCopy($originalExeBytes, 0, $changedExeBytes, 0, $originalExeBytes.Length)
    $changedExeBytes[$changedExeBytes.Length - 1] = 0x7F
    [IO.File]::WriteAllBytes($clientExe, $changedExeBytes)
    $changedExeRestore = Invoke-UcalDeployment @('-Action', 'Restore',
        '-TargetExecutable', $clientExe, '-BackupDirectory', $backup)
    Assert-Ucal ($changedExeRestore.ExitCode -ne 0) `
        'Restore accepted an executable changed after Apply.'
    [IO.File]::WriteAllBytes($clientExe, $originalExeBytes)

    $apiTarget = Join-Path $clientRoot $dll64
    Write-UcalUtf8 $apiTarget 'tampered UCAL API after Apply'
    $tamperedHash = (Get-FileHash $apiTarget -Algorithm SHA256).Hash
    $tamperedRestore = Invoke-UcalDeployment @('-Action', 'Restore',
        '-TargetExecutable', $clientExe, '-BackupDirectory', $backup)
    Assert-Ucal ($tamperedRestore.ExitCode -ne 0) 'Restore overwrote a changed deployed DLL.'
    Assert-Ucal ((Get-FileHash $apiTarget -Algorithm SHA256).Hash -eq $tamperedHash) `
        'Rejected Restore mutated the changed DLL.'
    Copy-Item -LiteralPath $frontend64 -Destination $apiTarget -Force

    $restore = Invoke-UcalDeployment @('-Action', 'Restore', '-TargetExecutable', $clientExe,
        '-BackupDirectory', $backup)
    Assert-Ucal ($restore.ExitCode -eq 0) "API Restore failed: $($restore.Text)"
    Assert-Ucal ((Compare-Object $baseline @(Get-UcalFingerprint $clientRoot)).Count -eq 0) `
        'Restore did not reproduce the exact original application file state.'
    $restoreAgain = Invoke-UcalDeployment @('-Action', 'Restore', '-TargetExecutable', $clientExe,
        '-BackupDirectory', $backup)
    Assert-Ucal ($restoreAgain.ExitCode -eq 0) 'Repeated API Restore was not idempotent.'

    $client32Root = Join-Path $testRoot 'client-x86'
    $client32Exe = Join-Path $client32Root 'sample-client.exe'
    $backup32 = Join-Path $testRoot 'backup-x86'
    New-UcalImportFixture $client32Exe -Machine x86
    $client32Hash = (Get-FileHash $client32Exe -Algorithm SHA256).Hash
    $apply32 = Invoke-UcalDeployment @('-Action', 'Apply', '-TargetExecutable', $client32Exe,
        '-FrontendDll', $frontend32, '-DiscoveryDll', $discovery32,
        '-BackupDirectory', $backup32)
    Assert-Ucal ($apply32.ExitCode -eq 0) "x86 API Apply failed: $($apply32.Text)"
    Assert-Ucal ((Get-FileHash $client32Exe -Algorithm SHA256).Hash -eq $client32Hash) `
        'x86 Apply modified the target executable.'
    $restore32 = Invoke-UcalDeployment @('-Action', 'Restore', '-TargetExecutable', $client32Exe,
        '-BackupDirectory', $backup32)
    Assert-Ucal ($restore32.ExitCode -eq 0) "x86 API Restore failed: $($restore32.Text)"
    Assert-Ucal (-not (Test-Path -LiteralPath (Join-Path $client32Root $dll32))) `
        'x86 Restore left a newly deployed frontend behind.'
    Assert-Ucal (-not (Test-Path -LiteralPath (Join-Path $client32Root $sidecar32))) `
        'x86 Restore left a newly deployed discovery sidecar behind.'

    if($Api -eq 'Vxl'){
        foreach($arch in @('x64','x86')){
            $legacyRoot=Join-Path $testRoot ('legacy-'+$arch)
            $legacyExe=Join-Path $legacyRoot 'client.exe'
            $legacyBackup=Join-Path $testRoot ('legacy-backup-'+$arch)
            New-UcalImportFixture $legacyExe -Machine $arch
            $front=if($arch -eq 'x64'){$frontend64}else{$frontend32}
            $discovery=if($arch -eq 'x64'){$discovery64}else{$discovery32}
            $legacyApply=Invoke-UcalDeployment @('-Action','Apply','-TargetExecutable',$legacyExe,
                '-FrontendDll',$front,'-DiscoveryDll',$discovery,'-BackupDirectory',$legacyBackup)
            Assert-Ucal ($legacyApply.ExitCode -eq 0) 'Legacy backup fixture Apply failed.'
            $legacyManifestPath=Join-Path $legacyBackup 'deployment.json'
            $legacyManifest=Get-Content -LiteralPath $legacyManifestPath -Raw | ConvertFrom-Json
            $legacyManifest.schema='org.ucal.vxl-client-deployment-backup/1'
            $supportName=if($arch -eq 'x64'){'UcalVectorSupport64.dll'}else{'UcalVectorSupport.dll'}
            $legacyManifest.entries=@($legacyManifest.entries | Where-Object {$_.relativePath -ne $supportName})
            Write-UcalUtf8 $legacyManifestPath ($legacyManifest | ConvertTo-Json -Depth 6)
            # A later independent file is outside the earlier two-file transaction.
            Write-UcalUtf8 (Join-Path $legacyRoot $supportName) 'independent later support file'
            $legacyRestore=Invoke-UcalDeployment @('-Action','Restore','-TargetExecutable',$legacyExe,'-BackupDirectory',$legacyBackup)
            Assert-Ucal ($legacyRestore.ExitCode -eq 0) "Legacy two-file Restore failed: $($legacyRestore.Text)"
            Assert-Ucal ((Get-Content -LiteralPath (Join-Path $legacyRoot $supportName) -Raw) -eq 'independent later support file') 'Legacy Restore changed an unowned support file.'
            Assert-Ucal (@(Get-ChildItem -LiteralPath $legacyRoot -File).Count -eq 2) 'Legacy Restore left an old transaction file behind.'
        }
    }

    $fault = Invoke-UcalDeployment @('-Action', 'Apply', '-TargetExecutable', $clientExe,
        '-FrontendDll', $frontend64, '-DiscoveryDll', $discovery64,
        '-BackupDirectory', $faultBackup, '-InjectFailureAfterFile', '1')
    Assert-Ucal ($fault.ExitCode -ne 0) 'Injected API deployment failure unexpectedly succeeded.'
    Assert-Ucal ((Compare-Object $baseline @(Get-UcalFingerprint $clientRoot)).Count -eq 0) `
        'Failed API Apply did not roll back exactly.'

    $noImportExe = Join-Path $testRoot 'negative-no-import\client.exe'
    New-UcalImportFixture $noImportExe -NoImports
    $noImport = Invoke-UcalDeployment @('-Action', 'Inspect', '-TargetExecutable', $noImportExe)
    Assert-Ucal ($noImport.ExitCode -ne 0) 'A client without a API import was accepted.'

    $missingRoot = Join-Path $testRoot 'negative-missing-export'
    $missingExe = Join-Path $missingRoot 'client.exe'
    New-UcalImportFixture $missingExe -Symbols @($entry, 'DefinitelyMissingApiFunction')
    $missing = Invoke-UcalDeployment @('-Action', 'Apply', '-TargetExecutable', $missingExe,
        '-FrontendDll', $frontend64, '-DiscoveryDll', $discovery64,
        '-BackupDirectory', (Join-Path $testRoot 'backup-missing'))
    Assert-Ucal ($missing.ExitCode -ne 0) 'A frontend missing an imported XL function was accepted.'

    $mismatch = Invoke-UcalDeployment @('-Action', 'Apply', '-TargetExecutable', $clientExe,
        '-FrontendDll', $frontend32, '-DiscoveryDll', $discovery64,
        '-BackupDirectory', (Join-Path $testRoot 'backup-mismatch'))
    Assert-Ucal ($mismatch.ExitCode -ne 0) 'An x86 frontend was accepted for an x64 client.'

    [ordered]@{
        schema = 'org.ucal.api-client-deployment-acceptance/1'
        api = $profile.id
        status = 'passed'
        capabilityCases = @('static-import-detected', 'no-import-rejected',
            'missing-export-rejected', 'architecture-mismatch-rejected')
        transactionCases = @('executable-unchanged', 'executable-change-refused',
            'deployed-file-change-refused', 'exact-restore', 'idempotent-restore',
            'fault-injected-apply-rollback')
        targetMachines = @('x86', 'x64')
        legacyBackupRestore = if($Api -eq 'Vxl'){'passed: x86/x64 two-file backups preserve later unowned support DLL'}else{'not-applicable'}
    } | ConvertTo-Json -Depth 5
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedCleanup = [IO.Path]::GetFullPath($testRoot).TrimEnd('\')
        if (-not $resolvedCleanup.StartsWith($outRoot + '\',
                [StringComparison]::OrdinalIgnoreCase) -or
            $resolvedCleanup -eq $outRoot) {
            throw 'Refusing unsafe API acceptance fixture cleanup.'
        }
        Remove-Item -LiteralPath $resolvedCleanup -Recurse -Force
    }
}

# Expected negative child-process cases leave LASTEXITCODE nonzero even after every
# assertion passes. Normalize only after the complete acceptance and safe cleanup.
$global:LASTEXITCODE = 0
