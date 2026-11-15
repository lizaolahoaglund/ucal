[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('PEAK', 'Vector')][string]$Vendor,
    [Parameter(Mandatory = $true)][string]$RuntimePath,
    [Parameter(Mandatory = $true)][string]$PackageRoot
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../../ucal/tools/UcalPeContract.psm1') -Force

$provider = @{
    PEAK   = @{Id='peak-j2534'; Name='PEAK PCAN-PassThru'; Runtime='PCANPT32.dll'
               Discovery='UcalJ2534PeakDiscovery.dll'; Layout='native'}
    Vector = @{Id='vector-j2534'; Name='Vector PassThru XL'; Runtime='VectorPassThruXL.dll'
               Discovery='UcalJ2534VectorDiscovery.dll'; Layout='packed'}
}[$Vendor]
$runtime = Get-Item -LiteralPath $RuntimePath
if ($runtime.PSIsContainer -or $runtime.Name -ine $provider.Runtime) {
    throw "Select the vendor's installed J2534 04.04 runtime ($($provider.Runtime))."
}
$contract = Get-UcalPeContract -Path $runtime.FullName
$standardExports = @('Open', 'Close', 'Connect', 'Disconnect', 'ReadMsgs', 'WriteMsgs',
    'StartPeriodicMsg', 'StopPeriodicMsg', 'StartMsgFilter', 'StopMsgFilter',
    'SetProgrammingVoltage', 'ReadVersion', 'GetLastError', 'Ioctl') |
    ForEach-Object { 'PassThru' + $_ }
if ($contract.Machine -notin @('x86','x64') -or
    @($standardExports | Where-Object { $_ -cnotin $contract.Exports }).Count) {
    throw 'A Windows x86/x64 J2534 runtime with the standard entry points is required.'
}

$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$arch = $contract.Machine
$discovery = Join-Path $root "runtime/$arch/discovery/$($provider.Discovery)"
$worker = Join-Path $root "runtime/$arch/UcalBroker.exe"
$adapter = Join-Path $root "runtime/$arch/UcalBackendJ2534.dll"
foreach ($file in @($discovery, $worker, $adapter)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing matching UCAL runtime file: $file"
    }
    if ((Get-UcalPeContract -Path $file -ImportsOnly).Machine -ne $arch) {
        throw "Unexpected architecture for UCAL runtime file: $file"
    }
}

$manifest = [ordered]@{
    schema = 'org.ucal.backend-package/5'
    id = $provider.Id
    name = $provider.Name
    vendor = $Vendor
    version = $runtime.VersionInfo.FileVersion
    type = 'j2534-0404-gmw17753'
    library = $runtime.FullName
    discovery_library = "../../runtime/$arch/discovery/$($provider.Discovery)"
    config_list_layout = $provider.Layout
}
if (-not $manifest.version) { throw 'The vendor DLL does not declare a file version.' }

$managerManifestPath = Join-Path $root 'UcalManagerPackage.json'
if (Test-Path -LiteralPath $managerManifestPath -PathType Leaf) {
    $managerPackage = Get-Content -LiteralPath $managerManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($managerPackage.schema -in @('org.ucal.manager-portable-package/1', 'org.ucal.manager-portable-package/2') -and
        $managerPackage.version -is [string] -and $managerPackage.version -cmatch '\A[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}\z') {
        $manifest.manager_version = $managerPackage.version
    }
}

$directory = Join-Path $root "backends/$($provider.Id)"
$destination = Join-Path $directory 'backend.json'
$json = ($manifest | ConvertTo-Json -Depth 4) + [Environment]::NewLine
if (Test-Path -LiteralPath $destination) {
    if ((Get-Content -LiteralPath $destination -Raw -Encoding UTF8) -cne $json) {
        throw 'A different backend manifest already exists. Move it aside before creating a replacement.'
    }
} else {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $data = [Text.Encoding]::UTF8.GetBytes($json)
        $stream.Write($data, 0, $data.Length)
    } finally {
        $stream.Dispose()
    }
}
$destination
