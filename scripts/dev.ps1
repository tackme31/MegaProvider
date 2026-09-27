# Build, then open a fresh pwsh with the module loaded and the location set to mega:.
# A fresh process each time: a loaded assembly can't be unloaded, and it locks the DLL against the next build.
param(
    [string]$Configuration = 'Debug',
    [switch]$NoShell,
    # Use the in-memory tree instead of MEGAcmd (no account needed).
    [switch]$Fake
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

dotnet build (Join-Path $root 'MegaProvider.sln') -c $Configuration -nologo -v q
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$psd1 = Join-Path $root "src/MegaProvider/bin/$Configuration/net8.0/MegaProvider.psd1"
if ($NoShell) { $psd1; return }
$backend = if ($Fake) { "`$env:MEGAPROVIDER_BACKEND = 'fake'; " } else { '' }
pwsh -NoLogo -NoExit -Command "$backend Import-Module '$psd1'; Set-Location mega:"
