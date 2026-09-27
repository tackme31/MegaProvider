<#
.SYNOPSIS
    Build MegaProvider in Release, zip the module, and check the zip works on its own.

.DESCRIPTION
    dev.ps1 -Configuration Release -Native (host and module) -> stage the module with
    the MSVC runtime, LICENSE and a generated THIRD-PARTY-NOTICES.txt -> zip -> check
    the entries -> unpack into a temp module folder and, in a fresh pwsh, import it
    and list mega:\ on the fake backend.

    The zip's root is the module folder's contents, so it unpacks straight into
    Modules\MegaProvider\<version>. Output: artifacts\MegaProvider-<version>-win-x64.zip.

.PARAMETER SkipBuild
    Package whatever is already built in Release. Fails if it is missing.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $root 'src/MegaProvider/bin/Release/net8.0'

# ---------------------------------------------------------------------- build
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'dev.ps1') -Configuration Release -Native -NoShell | Out-Null
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}
$psd1 = Join-Path $outDir 'MegaProvider.psd1'
if (-not (Test-Path $psd1)) { throw "$psd1 not found -- build first" }
$version = (Import-PowerShellDataFile $psd1).ModuleVersion

# -------------------------------------------------------------------- staging
$artifacts = Join-Path $root 'artifacts'
$stage = Join-Path $artifacts 'MegaProvider'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($name in 'MegaProvider.psd1', 'MegaProvider.dll', 'MegaProvider.format.ps1xml', 'megaprovider-host.exe') {
    Copy-Item (Join-Path $outDir $name) $stage
}
Copy-Item (Join-Path $root 'LICENSE') $stage

# The host links the CRT dynamically (the SDK's overlay triplet); ship it app-local so
# a machine without the VC++ redistributable still starts it. The newest redist serves
# binaries built with older toolsets (v142 here).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory |
    Sort-Object { [version]$_.Parent.Parent.Name } | Select-Object -Last 1
if (-not $crt) { throw 'MSVC redist (Microsoft.VC14*.CRT) not found' }
foreach ($name in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt.FullName $name) $stage
}

# ------------------------------------------------------------------- notices
# Every vcpkg port the SDK pulled in is linked statically into the host, so each one's
# copyright travels. Ports without a copyright file are build tools (vcpkg-cmake, pkgconf).
function New-ThirdPartyNotices {
    $native = Join-Path $root 'native'
    $installed = Join-Path $native 'build/vcpkg_installed'
    $banner = '=' * 79

    $ports = foreach ($block in ((Get-Content -Raw (Join-Path $installed 'vcpkg/status')) -split "\r?\n\r?\n")) {
        $f = @{}
        foreach ($line in $block -split "\r?\n") { if ($line -match '^([^:]+): (.*)$') { $f[$Matches[1]] = $Matches[2] } }
        if (-not $f.Count -or $f.ContainsKey('Feature') -or $f['Architecture'] -ne 'x64-windows-mega') { continue }
        if ($f['Status'] -ne 'install ok installed') { continue }
        $copyright = Join-Path $installed "x64-windows-mega/share/$($f['Package'])/copyright"
        if (-not (Test-Path $copyright)) { continue }
        $ver = $f['Version'] + $(if ($f['Port-Version']) { "#$($f['Port-Version'])" } else { '' })
        [pscustomobject]@{ Name = $f['Package']; Version = $ver; Text = Get-Content -Raw $copyright }
    }
    $sdkRev = git -C (Join-Path $native 'third_party/sdk') describe --tags --always
    $jsonVer = if ((Get-Content (Join-Path $native 'third_party/json/nlohmann/json.hpp') -TotalCount 5) -join "`n" -match 'version (\d+\.\d+\.\d+)') { $Matches[1] } else { '?' }

    $components = @(
        [pscustomobject]@{ Name = 'MEGA C++ SDK'; Version = $sdkRev; Text = Get-Content -Raw (Join-Path $native 'third_party/sdk/LICENSE') }
        [pscustomobject]@{ Name = 'nlohmann/json'; Version = $jsonVer; Text = Get-Content -Raw (Join-Path $native 'third_party/json/LICENSE.MIT') }
    ) + @($ports | Sort-Object Name)

    $sb = [Text.StringBuilder]::new()
    [void]$sb.AppendLine('MegaProvider')
    [void]$sb.AppendLine('Third-Party Software Notices and Information')
    [void]$sb.AppendLine()
    [void]$sb.AppendLine('MegaProvider is licensed under the MIT License (see LICENSE). Source:')
    [void]$sb.AppendLine('https://github.com/tackme31/MegaProvider')
    [void]$sb.AppendLine()
    [void]$sb.AppendLine('megaprovider-host.exe statically links the components below. The files')
    [void]$sb.AppendLine('msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll are the Microsoft Visual')
    [void]$sb.AppendLine('C++ runtime, redistributed under the Visual Studio license terms.')
    [void]$sb.AppendLine('Generated by scripts/package.ps1.')
    [void]$sb.AppendLine()
    foreach ($c in $components) { [void]$sb.AppendLine("  $($c.Name) $($c.Version)") }
    foreach ($c in $components) {
        [void]$sb.AppendLine().AppendLine($banner).AppendLine("$($c.Name) $($c.Version)").AppendLine($banner).AppendLine()
        [void]$sb.AppendLine($c.Text.Replace("`r`n", "`n").TrimEnd())
    }
    $sb.ToString().Replace("`r`n", "`n")
}
[IO.File]::WriteAllText((Join-Path $stage 'THIRD-PARTY-NOTICES.txt'), (New-ThirdPartyNotices))

# ------------------------------------------------------------------------ zip
$zip = Join-Path $artifacts "MegaProvider-$version-win-x64.zip"
Remove-Item (Join-Path $artifacts '*.zip') -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
$required = 'MegaProvider.psd1', 'MegaProvider.dll', 'MegaProvider.format.ps1xml', 'megaprovider-host.exe',
    'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE', 'THIRD-PARTY-NOTICES.txt'
$missing = $required | Where-Object { $_ -notin $entries }
if ($missing) { throw "zip is missing: $($missing -join ', ')" }

# ---------------------------------------------------------------- import check
# A module folder named like the real install, loaded by name through PSModulePath.
$check = Join-Path ([IO.Path]::GetTempPath()) 'MegaProvider-package-check'
Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
Expand-Archive $zip (Join-Path $check "MegaProvider/$version")
$script = @'
$ErrorActionPreference = 'Stop'
$env:MEGAPROVIDER_BACKEND = 'fake'
Import-Module MegaProvider
$m = Get-Module MegaProvider
if ($m.ModuleBase -notlike "$env:MP_CHECK*") { throw "loaded from $($m.ModuleBase)" }
if (@($m.ExportedCmdlets.Keys).Count -lt 7) { throw 'cmdlets missing' }
if (-not @(Get-ChildItem mega:\).Count) { throw 'mega:\ is empty' }
'ok'
'@
$env:MP_CHECK = $check
$savedModulePath = $env:PSModulePath
$env:PSModulePath = "$check;$env:PSModulePath"
try {
    $result = pwsh -NoProfile -NonInteractive -Command $script
} finally {
    $env:PSModulePath = $savedModulePath
    Remove-Item env:MP_CHECK
}
if ($LASTEXITCODE -or $result -ne 'ok') { throw "import check failed: $result" }
Remove-Item -Recurse -Force $check

$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "$zip ($sizeMb MB, $($entries.Count) entries; imported and listed mega:\ on the fake backend)" -ForegroundColor Green
