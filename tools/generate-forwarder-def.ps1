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

# Compatibility entry point; shared generator belongs to the independent UCAL library.
& (Join-Path $PSScriptRoot '../ucal/tools/generate-forwarder-def.ps1') @PSBoundParameters
