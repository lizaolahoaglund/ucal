[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Inspect','Apply','Restore')][string]$Action,
    [Parameter(Mandatory=$true)][string]$TargetExecutable,
    [string]$FrontendDll,
    [string]$BackupDirectory,
    [ValidateRange(0,2)][int]$InjectFailureAfterFile = 0
)
$ErrorActionPreference='Stop'
$moduleName='UcalApiDeployment.psm1'
$module=Join-Path $PSScriptRoot $moduleName
if (-not (Test-Path -LiteralPath $module)) { $module=Join-Path $PSScriptRoot '../../../ucal/tools/UcalApiDeployment.psm1' }
Import-Module $module -Force
$profile=Join-Path $PSScriptRoot 'deployment.json'
if (-not (Test-Path -LiteralPath $profile)) { $profile=Join-Path $PSScriptRoot '../deployment.json' }
Invoke-UcalApiDeployment -Action $Action -TargetExecutable $TargetExecutable -ProfilePath $profile `
    -SourcesByRole @{FrontendDll=$FrontendDll} -BackupDirectory $BackupDirectory -InjectFailureAfterFile $InjectFailureAfterFile
