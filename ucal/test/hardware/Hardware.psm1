function Get-UcalHardwareBackendMetadata {
    param([string]$Root)
    foreach ($folder in (Get-ChildItem -LiteralPath (Join-Path $Root 'backends') -Directory -ErrorAction SilentlyContinue)) {
        $path = Join-Path $folder.FullName 'backend.json'
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $metadata = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
            [pscustomobject]@{ Directory = $folder.FullName; Metadata = $metadata }
        }
    }
}
function Get-UcalHardwareTestProfile {
    param([string]$Root, [string]$Backend)
    if ($Backend -eq 'J2534') {
        return [pscustomobject]@{ runtime_environment=''; requires_termination=$false;
            supports_termination=$true; requires_tx_completion=$false;
            timing_readback='exact-public-runtime-readback'; supports_tx_completion=$false;
            submission_correlated=$false; filter_rejection_requires_self_receive=$true; echo_environment='' }
    }
    $module = Get-UcalHardwareBackendMetadata $Root | Where-Object {
        $Backend -in (@($_.Metadata.id) + @($_.Metadata.broker_aliases))
    } | Select-Object -Last 1
    if (-not $module) { throw "Backend selector is not provided by an installed module: $Backend" }
    $path = Join-Path $module.Directory 'test/hardware.json'
    if (-not (Test-Path -LiteralPath $path)) { throw "Module has no hardware acceptance profile: $Backend" }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}
function Get-UcalHardwareRuntimeEnvironmentNames {
    param([string]$Root)
    foreach ($module in (Get-UcalHardwareBackendMetadata $Root)) {
        $path = Join-Path $module.Directory 'test/hardware.json'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $profile = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($setting in @('runtime_environment','echo_environment')) {
            if ($profile.$setting) { [string]$profile.$setting }
        }
    }
}
Export-ModuleMember -Function Get-UcalHardwareTestProfile,Get-UcalHardwareRuntimeEnvironmentNames
