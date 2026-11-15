Set-StrictMode -Version Latest

function Assert-UcalPeRange {
    param([byte[]]$Bytes, [long]$Offset, [long]$Length, [string]$Label)
    if ($Offset -lt 0 -or $Length -lt 0 -or $Offset -gt $Bytes.LongLength -or
        $Length -gt ($Bytes.LongLength - $Offset)) {
        throw "Invalid PE $Label range: offset=$Offset length=$Length size=$($Bytes.LongLength)"
    }
}

function Get-UcalPeUInt16([byte[]]$Bytes, [long]$Offset, [string]$Label) {
    Assert-UcalPeRange $Bytes $Offset 2 $Label
    return [BitConverter]::ToUInt16($Bytes, [int]$Offset)
}

function Get-UcalPeUInt32([byte[]]$Bytes, [long]$Offset, [string]$Label) {
    Assert-UcalPeRange $Bytes $Offset 4 $Label
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Get-UcalPeUInt64([byte[]]$Bytes, [long]$Offset, [string]$Label) {
    Assert-UcalPeRange $Bytes $Offset 8 $Label
    return [BitConverter]::ToUInt64($Bytes, [int]$Offset)
}

function Get-UcalPeAscii([byte[]]$Bytes, [long]$Offset, [string]$Label) {
    Assert-UcalPeRange $Bytes $Offset 1 $Label
    $end = $Offset
    $maximum = [Math]::Min($Bytes.LongLength, $Offset + 4096)
    while ($end -lt $maximum -and $Bytes[[int]$end] -ne 0) { $end++ }
    if ($end -ge $maximum) { throw "PE $Label string is not terminated within 4096 bytes." }
    return [Text.Encoding]::ASCII.GetString($Bytes, [int]$Offset, [int]($end - $Offset))
}

function Convert-UcalPeRva {
    param(
        [byte[]]$Bytes,
        [uint32]$Rva,
        [uint32]$SizeOfHeaders,
        [object[]]$Sections,
        [string]$Label
    )
    if ($Rva -lt $SizeOfHeaders) {
        Assert-UcalPeRange $Bytes $Rva 1 $Label
        return [long]$Rva
    }
    foreach ($section in $Sections) {
        $span = [Math]::Max([uint64]$section.VirtualSize, [uint64]$section.RawSize)
        $start = [uint64]$section.VirtualAddress
        $value = [uint64]$Rva
        if ($value -ge $start -and $value -lt ($start + $span)) {
            $delta = $value - $start
            if ($delta -ge [uint64]$section.RawSize) {
                throw "PE $Label RVA points into uninitialized section data."
            }
            $offset = [uint64]$section.RawPointer + $delta
            Assert-UcalPeRange $Bytes ([long]$offset) 1 $Label
            return [long]$offset
        }
    }
    throw ('PE {0} RVA 0x{1:X8} is outside all file-backed sections.' -f $Label, $Rva)
}

function Get-UcalPeContract {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path, [switch]$ImportsOnly)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $bytes = [IO.File]::ReadAllBytes($resolved)
    if ((Get-UcalPeUInt16 $bytes 0 'DOS signature') -ne 0x5A4D) {
        throw "Not a PE image: $resolved"
    }
    $peOffset = Get-UcalPeUInt32 $bytes 0x3C 'PE header pointer'
    Assert-UcalPeRange $bytes $peOffset 24 'PE header'
    if ((Get-UcalPeUInt32 $bytes $peOffset 'PE signature') -ne 0x00004550) {
        throw "Invalid PE signature: $resolved"
    }
    $machineValue = Get-UcalPeUInt16 $bytes ($peOffset + 4) 'COFF machine'
    $machine = switch ($machineValue) {
        0x014C { 'x86' }
        0x8664 { 'x64' }
        0xAA64 { 'arm64' }
        default { 'unknown' }
    }
    $sectionCount = Get-UcalPeUInt16 $bytes ($peOffset + 6) 'section count'
    if ($sectionCount -lt 1 -or $sectionCount -gt 96) {
        throw "Unsupported PE section count: $sectionCount"
    }
    $optionalSize = Get-UcalPeUInt16 $bytes ($peOffset + 20) 'optional header size'
    $optional = [long]$peOffset + 24
    Assert-UcalPeRange $bytes $optional $optionalSize 'optional header'
    $magic = Get-UcalPeUInt16 $bytes $optional 'optional header magic'
    if ($magic -eq 0x10B) {
        $is64 = $false
        $directoryCountOffset = $optional + 92
        $directoryOffset = $optional + 96
    }
    elseif ($magic -eq 0x20B) {
        $is64 = $true
        $directoryCountOffset = $optional + 108
        $directoryOffset = $optional + 112
    }
    else {
        throw ('Unsupported PE optional header magic: 0x{0:X4}' -f $magic)
    }
    if (($machine -eq 'x64') -ne $is64) {
        throw "PE machine and optional-header bitness disagree: $resolved"
    }
    $sizeOfHeaders = Get-UcalPeUInt32 $bytes ($optional + 60) 'SizeOfHeaders'
    $directoryCount = Get-UcalPeUInt32 $bytes $directoryCountOffset 'data directory count'
    $sectionTable = $optional + $optionalSize
    Assert-UcalPeRange $bytes $sectionTable ([long]$sectionCount * 40) 'section table'
    $sections = @()
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $section = $sectionTable + ($index * 40)
        $sections += [pscustomobject]@{
            VirtualSize = Get-UcalPeUInt32 $bytes ($section + 8) 'section virtual size'
            VirtualAddress = Get-UcalPeUInt32 $bytes ($section + 12) 'section RVA'
            RawSize = Get-UcalPeUInt32 $bytes ($section + 16) 'section raw size'
            RawPointer = Get-UcalPeUInt32 $bytes ($section + 20) 'section raw pointer'
        }
    }

    $imports = @()
    if ($directoryCount -gt 1) {
        Assert-UcalPeRange $bytes ($directoryOffset + 8) 8 'import directory entry'
        $importRva = Get-UcalPeUInt32 $bytes ($directoryOffset + 8) 'import directory RVA'
        if ($importRva -ne 0) {
            $descriptor = Convert-UcalPeRva $bytes $importRva $sizeOfHeaders $sections 'import directory'
            for ($descriptorIndex = 0; $descriptorIndex -lt 1024; $descriptorIndex++) {
                Assert-UcalPeRange $bytes $descriptor 20 'import descriptor'
                $originalThunk = Get-UcalPeUInt32 $bytes $descriptor 'OriginalFirstThunk'
                $nameRva = Get-UcalPeUInt32 $bytes ($descriptor + 12) 'import DLL name RVA'
                $firstThunk = Get-UcalPeUInt32 $bytes ($descriptor + 16) 'FirstThunk'
                if ($originalThunk -eq 0 -and $nameRva -eq 0 -and $firstThunk -eq 0) { break }
                if ($nameRva -eq 0) { throw 'PE import descriptor omits its DLL name.' }
                $nameOffset = Convert-UcalPeRva $bytes $nameRva $sizeOfHeaders $sections 'import DLL name'
                $dllName = Get-UcalPeAscii $bytes $nameOffset 'import DLL name'
                $symbols = @()
                $thunkRva = if ($originalThunk -ne 0) { $originalThunk } else { $firstThunk }
                if ($thunkRva -ne 0) {
                    $thunkOffset = Convert-UcalPeRva $bytes $thunkRva $sizeOfHeaders $sections 'import thunk'
                    $thunkWidth = if ($is64) { 8 } else { 4 }
                    for ($thunkIndex = 0; $thunkIndex -lt 4096; $thunkIndex++) {
                        $value = if ($is64) {
                            Get-UcalPeUInt64 $bytes $thunkOffset 'import thunk value'
                        } else {
                            [uint64](Get-UcalPeUInt32 $bytes $thunkOffset 'import thunk value')
                        }
                        if ($value -eq 0) { break }
                        $isOrdinal = if ($is64) { ($value -shr 63) -ne 0 } else {
                            ($value -shr 31) -ne 0
                        }
                        if ($isOrdinal) {
                            $symbols += ('#{0}' -f ($value -band 0xFFFF))
                        }
                        else {
                            if ($value -gt [uint32]::MaxValue) {
                                throw 'PE import-by-name RVA exceeds the supported 32-bit image range.'
                            }
                            $symbolOffset = Convert-UcalPeRva $bytes ([uint32]$value) `
                                $sizeOfHeaders $sections 'import symbol'
                            $symbols += Get-UcalPeAscii $bytes ($symbolOffset + 2) 'import symbol'
                        }
                        $thunkOffset += $thunkWidth
                    }
                }
                $imports += [pscustomobject]@{ Dll = $dllName; Symbols = @($symbols) }
                $descriptor += 20
            }
        }
    }

    $exports = @()
    if ($directoryCount -gt 0 -and -not $ImportsOnly) {
        Assert-UcalPeRange $bytes $directoryOffset 8 'export directory entry'
        $exportRva = Get-UcalPeUInt32 $bytes $directoryOffset 'export directory RVA'
        if ($exportRva -ne 0) {
            $exportOffset = Convert-UcalPeRva $bytes $exportRva $sizeOfHeaders $sections 'export directory'
            Assert-UcalPeRange $bytes $exportOffset 40 'export directory'
            $nameCount = Get-UcalPeUInt32 $bytes ($exportOffset + 24) 'export name count'
            if ($nameCount -gt 65536) { throw "Unsupported PE export-name count: $nameCount" }
            $namesRva = Get-UcalPeUInt32 $bytes ($exportOffset + 32) 'export names RVA'
            if ($nameCount -ne 0 -and $namesRva -eq 0) { throw 'PE export names table is missing.' }
            if ($nameCount -ne 0) {
                $namesOffset = Convert-UcalPeRva $bytes $namesRva $sizeOfHeaders $sections 'export names table'
                for ($nameIndex = 0; $nameIndex -lt $nameCount; $nameIndex++) {
                    $nameRva = Get-UcalPeUInt32 $bytes ($namesOffset + $nameIndex * 4) 'export name RVA'
                    $nameOffset = Convert-UcalPeRva $bytes $nameRva $sizeOfHeaders $sections 'export name'
                    $exports += Get-UcalPeAscii $bytes $nameOffset 'export name'
                }
            }
        }
    }

    return [pscustomobject]@{
        Path = $resolved
        Machine = $machine
        Imports = @($imports)
        Exports = @($exports | Sort-Object -Unique)
    }
}

Export-ModuleMember -Function Get-UcalPeContract
