# Build, then run the Pester tests, each suite in its own fresh pwsh: the loaded module can't be
# unloaded, and the backend is fixed per process (MEGAPROVIDER_BACKEND).
#
#   ./scripts/test.ps1                          # fake backend only (no account)
#   ./scripts/test.ps1 -Live                    # + the test account through the SDK host
#   ./scripts/test.ps1 -Live -Backend host, megacmd
#
# -Live needs the test account connected (Connect-MegaAccount) and MEGAEXPLORER_TEST_* set (CLAUDE.md).
param(
    [switch]$Live,
    [ValidateSet('host', 'megacmd')]
    [string[]]$Backend = @('host'),
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not (Get-Module Pester -ListAvailable | Where-Object Version -GE '5.5')) {
    throw 'Pester 5.5 or later is required: Install-Module Pester -Scope CurrentUser -SkipPublisherCheck'
}

$psd1 = if ($NoBuild) {
    Join-Path $root 'src/MegaProvider/bin/Debug/net8.0/MegaProvider.psd1'
} else {
    & (Join-Path $PSScriptRoot 'dev.ps1') -NoShell | Select-Object -Last 1
}

function Invoke-Suite([string]$file, [string]$backendValue, [string]$label) {
    Write-Host "`n=== $label ===" -ForegroundColor Cyan
    $script = @"
`$env:MEGAPROVIDER_PSD1 = '$psd1'
`$env:MEGAPROVIDER_BACKEND = '$backendValue'
Import-Module Pester -MinimumVersion 5.5
`$r = Invoke-Pester -Path '$(Join-Path $root "tests/$file")' -Output Detailed -PassThru
exit `$r.FailedCount + `$r.FailedBlocksCount + `$r.FailedContainersCount
"@
    pwsh -NoProfile -Command $script | Out-Host   # keep its output out of this function's return value
    return $LASTEXITCODE
}

$failed = Invoke-Suite 'Fake.Tests.ps1' 'fake' 'fake backend'
if ($Live) {
    foreach ($b in $Backend) {
        $failed += Invoke-Suite 'Live.Tests.ps1' ($b -eq 'host' ? '' : $b) "live: $b"
    }
}
Write-Host "`n$(if ($failed) { "FAILED: $failed" } else { 'All passed.' })" -ForegroundColor ($failed ? 'Red' : 'Green')
exit $failed
