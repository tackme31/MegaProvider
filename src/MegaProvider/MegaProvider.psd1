@{
    RootModule           = 'MegaProvider.dll'
    ModuleVersion        = '0.2.0'
    GUID                 = '6f1d2b1e-4c7a-4f0e-9a51-3d6e2c8b7a10'
    Author               = 'Takumi Yamada'
    Copyright            = '(c) 2026 Takumi Yamada. MIT License.'
    Description          = 'Exposes MEGA cloud storage as a PowerShell drive (mega:).'
    PowerShellVersion    = '7.4'
    CompatiblePSEditions = @('Core')
    FormatsToProcess     = @('MegaProvider.format.ps1xml')
    CmdletsToExport      = @(
        'Connect-MegaAccount', 'Get-MegaAccount', 'Disconnect-MegaAccount',
        'Send-MegaItem', 'Receive-MegaItem', 'Get-MegaRubbishItem', 'Restore-MegaItem'
    )
    FunctionsToExport    = @()
    AliasesToExport      = @()
    VariablesToExport    = @()
}
