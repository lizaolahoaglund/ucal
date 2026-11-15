function Get-UcalBrokerPipeName {
    $pipeName = 'ucal.discovery.v1.' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if ($env:UCAL_BROKER_INSTANCE) {
        if ($env:UCAL_BROKER_INSTANCE -cnotmatch '^[A-Za-z0-9_-]{1,64}$') {
            throw 'UCAL_BROKER_INSTANCE must contain 1-64 ASCII letters, digits, underscores or hyphens.'
        }
        $pipeName += '.' + $env:UCAL_BROKER_INSTANCE
    }
    return $pipeName
}
