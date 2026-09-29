<#
.SYNOPSIS
    Build MegaProvider in Release, package the module for this OS, and check the package works on its own.

.DESCRIPTION
    dev.ps1 -Configuration Release -Native (host and module) -> stage the module with LICENSE and a
    generated THIRD-PARTY-NOTICES.txt (and, on Windows, the MSVC runtime) -> archive -> check the
    entries -> unpack into a temp module folder and, in a fresh pwsh, import it and list mega:\ on the
    fake backend.

    The archive's root is the module folder's contents, so it unpacks straight into
    Modules/MegaProvider/<version>. Output in artifacts/:
      Windows  MegaProvider-<version>-win-x64.zip
      Linux    MegaProvider-<version>-linux-x64.tar.gz  (tar keeps the host's executable bit; zip would not)

.PARAMETER SkipBuild
    Package whatever is already built in Release. Fails if it is missing.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ($IsWindows -or $IsLinux)) { throw 'Packages are built on Windows or Linux.' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $root 'src/MegaProvider/bin/Release/net8.0'
$hostName = $IsWindows ? 'megaprovider-host.exe' : 'megaprovider-host'
$triplet = $IsWindows ? 'x64-windows-mega' : 'x64-linux-mega'
$platform = $IsWindows ? 'win-x64' : 'linux-x64'
# README promises this; a host built on a newer distro would quietly break it.
$maxGlibc = [version]'2.35'

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

foreach ($name in 'MegaProvider.psd1', 'MegaProvider.dll', 'MegaProvider.format.ps1xml', 'MegaProvider.types.ps1xml', $hostName) {
    Copy-Item (Join-Path $outDir $name) $stage
}
Copy-Item (Join-Path $outDir 'en-US') $stage -Recurse   # Get-Help (scripts/Build-HelpFile.ps1)
Copy-Item (Join-Path $root 'LICENSE') $stage
$required = @('MegaProvider.psd1', 'MegaProvider.dll', 'MegaProvider.format.ps1xml', 'MegaProvider.types.ps1xml', $hostName, 'LICENSE', 'THIRD-PARTY-NOTICES.txt',
    'en-US/MegaProvider.dll-Help.xml', 'en-US/about_MegaProvider.help.txt')

if ($IsWindows) {
    # The host links the CRT dynamically (the SDK's overlay triplet); ship it app-local so
    # a machine without the VC++ redistributable still starts it. The newest redist serves
    # binaries built with older toolsets (v142 here).
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory |
        Sort-Object { [version]$_.Parent.Parent.Name } | Select-Object -Last 1
    if (-not $crt) { throw 'MSVC redist (Microsoft.VC14*.CRT) not found' }
    foreach ($name in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
        Copy-Item (Join-Path $crt.FullName $name) $stage
        $required += $name
    }
} else {
    # libstdc++ and libgcc are linked in statically (CMakeLists.txt), so glibc is all the host
    # needs from the system: check that nothing else crept in, and how new a glibc it wants.
    $hostPath = Join-Path $stage $hostName
    $libs = @(ldd $hostPath | ForEach-Object { if ($_ -match '^\s*(\S+\.so[.\d]*)') { $Matches[1] } })
    $unexpected = $libs | Where-Object { $_ -notmatch '(^|/)(linux-vdso|libc|libm|ld-linux-x86-64)\.so' }
    if ($unexpected) { throw "the host links more than glibc: $($unexpected -join ', ')" }
    $glibc = objdump -T $hostPath | Select-String -AllMatches 'GLIBC_(\d+\.\d+)' |
        ForEach-Object { $_.Matches } | ForEach-Object { [version]$_.Groups[1].Value } |
        Sort-Object -Unique | Select-Object -Last 1
    if ($glibc -gt $maxGlibc) { throw "the host needs glibc $glibc; README promises $maxGlibc. Build on an older distro." }
}

# ------------------------------------------------------------------- notices
# Every vcpkg port the SDK pulled in is linked statically into the host, so each one's
# copyright travels. Ports without a copyright file are build tools (vcpkg-cmake, pkgconf).
function New-ThirdPartyNotices {
    $native = Join-Path $root 'native'
    $installed = Join-Path $native 'build/vcpkg_installed'
    $banner = '=' * 79

    # Dual-licensed ports: take the non-(L)GPL option. Both options ask that recipients
    # be told where the (unmodified) source is.
    $choices = @{
        freeimage = 'Used under the FreeImage Public License 1.0 (not the GPL option). Source: https://freeimage.sourceforge.io/'
        libraw    = 'Used under the CDDL 1.0 (not the LGPL option). Source: https://www.libraw.org/'
    }

    $ports = foreach ($block in ((Get-Content -Raw (Join-Path $installed 'vcpkg/status')) -split "\r?\n\r?\n")) {
        $f = @{}
        foreach ($line in $block -split "\r?\n") { if ($line -match '^([^:]+): (.*)$') { $f[$Matches[1]] = $Matches[2] } }
        if (-not $f.Count -or $f.ContainsKey('Feature') -or $f['Architecture'] -ne $triplet) { continue }
        if ($f['Status'] -ne 'install ok installed') { continue }
        $copyright = Join-Path $installed "$triplet/share/$($f['Package'])/copyright"
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
    [void]$sb.AppendLine("$hostName statically links the components below.")
    if ($IsWindows) {
        [void]$sb.AppendLine('The files msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll are the Microsoft')
        [void]$sb.AppendLine('Visual C++ runtime, redistributed under the Visual Studio license terms.')
    } else {
        [void]$sb.AppendLine('It also statically links the GCC runtime (libstdc++, libgcc), under the GCC')
        [void]$sb.AppendLine('Runtime Library Exception, which asks for no notice.')
    }
    [void]$sb.AppendLine('Generated by scripts/package.ps1.')
    [void]$sb.AppendLine()
    foreach ($c in $components) {
        $note = if ($choices.ContainsKey($c.Name)) { '  (non-GPL option chosen, see below)' } else { '' }
        [void]$sb.AppendLine("  $($c.Name) $($c.Version)$note")
    }
    foreach ($c in $components) {
        [void]$sb.AppendLine().AppendLine($banner).AppendLine("$($c.Name) $($c.Version)").AppendLine($banner).AppendLine()
        if ($choices.ContainsKey($c.Name)) { [void]$sb.AppendLine($choices[$c.Name]).AppendLine() }
        [void]$sb.AppendLine($c.Text.Replace("`r`n", "`n").TrimEnd())
    }
    $sb.ToString().Replace("`r`n", "`n")
}
[IO.File]::WriteAllText((Join-Path $stage 'THIRD-PARTY-NOTICES.txt'), (New-ThirdPartyNotices))

# -------------------------------------------------------------------- archive
# Only this platform's earlier packages go: the other one may sit next to it for the release.
if ($IsWindows) {
    $package = Join-Path $artifacts "MegaProvider-$version-win-x64.zip"
    Remove-Item (Join-Path $artifacts 'MegaProvider-*-win-x64.zip') -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $package

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($package)
    try { $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') }) } finally { $archive.Dispose() }
} else {
    $package = Join-Path $artifacts "MegaProvider-$version-linux-x64.tar.gz"
    Remove-Item (Join-Path $artifacts 'MegaProvider-*-linux-x64.tar.gz') -Force -ErrorAction SilentlyContinue
    # Plain names at the root (no ./ prefix), owned by nobody in particular.
    tar -czf $package -C $stage --owner=0 --group=0 @(Get-ChildItem $stage -Name)
    if ($LASTEXITCODE) { throw "tar failed ($LASTEXITCODE)" }

    $listing = @(tar -tvzf $package)
    $entries = @($listing | ForEach-Object { ($_ -split '\s+')[-1] })
    if (-not ($listing | Where-Object { $_ -match '^-rwx' -and $_ -match " $hostName$" })) {
        throw "$hostName lost its executable bit in the archive"
    }
}
$missing = $required | Where-Object { $_ -notin $entries }
if ($missing) { throw "package is missing: $($missing -join ', ')" }

# ---------------------------------------------------------------- import check
# A module folder named like the real install, loaded by name through PSModulePath.
$check = Join-Path ([IO.Path]::GetTempPath()) 'MegaProvider-package-check'
Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
$moduleDir = Join-Path $check "MegaProvider/$version"
if ($IsWindows) {
    Expand-Archive $package $moduleDir
} else {
    New-Item -ItemType Directory -Force $moduleDir | Out-Null
    tar -xzf $package -C $moduleDir
    if ($LASTEXITCODE) { throw "tar failed ($LASTEXITCODE)" }
}
$script = @'
$ErrorActionPreference = 'Stop'
$env:MEGAPROVIDER_BACKEND = 'fake'
Import-Module MegaProvider
$m = Get-Module MegaProvider
if ($m.ModuleBase -notlike "$env:MP_CHECK*") { throw "loaded from $($m.ModuleBase)" }
if (@($m.ExportedCmdlets.Keys).Count -lt 7) { throw 'cmdlets missing' }
if (-not @(Get-ChildItem mega:\).Count) { throw 'mega:\ is empty' }
if (-not (Get-Help Publish-MegaItem).Synopsis.StartsWith('Creates')) { throw 'Get-Help finds no help' }
if (-not (Get-Help about_MegaProvider)) { throw 'Get-Help finds no about_MegaProvider' }
'ok'
'@
$env:MP_CHECK = $check
$savedModulePath = $env:PSModulePath
$env:PSModulePath = $check + [IO.Path]::PathSeparator + $env:PSModulePath
try {
    $result = pwsh -NoProfile -NonInteractive -Command $script
} finally {
    $env:PSModulePath = $savedModulePath
    Remove-Item env:MP_CHECK
}
if ($LASTEXITCODE -or $result -ne 'ok') { throw "import check failed: $result" }
Remove-Item -Recurse -Force $check

$sizeMb = [math]::Round((Get-Item $package).Length / 1MB, 1)
$glibcNote = $IsLinux ? "; needs glibc $glibc or later" : ''
Write-Host "$package ($sizeMb MB, $($entries.Count) entries$glibcNote; imported, listed mega:\ on the fake backend and read its help)" -ForegroundColor Green
