# Build, then open a fresh pwsh with the module loaded and the location set to mega:.
# A fresh process each time: a loaded assembly can't be unloaded, and it locks the DLL against the next build.
param(
    [string]$Configuration = 'Debug',
    [switch]$NoShell,
    # Use the in-memory tree instead of MEGA (no account needed).
    [switch]$Fake,
    # Also (re)build the native host. Done automatically when it has never been built.
    [switch]$Native
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# The running host holds megaprovider-host.exe open in the module output, so the copy would fail.
function Stop-Host {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', (& "$PSScriptRoot/Get-HostPipeName.ps1"), [IO.Pipes.PipeDirection]::InOut)
    try { $pipe.Connect(200) } catch [TimeoutException] { return }
    try {
        $writer = [IO.StreamWriter]::new($pipe); $writer.NewLine = "`n"
        $writer.WriteLine('{"id":1,"op":"shutdown"}'); $writer.Flush()
        $reply = [IO.StreamReader]::new($pipe).ReadLine() | ConvertFrom-Json
        if (-not $reply.ok) { throw "The MEGA host did not stop: $($reply.error.message). Try again when it finishes." }
    } finally { $pipe.Dispose() }
    Get-Process megaprovider-host -ErrorAction SilentlyContinue | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}
Stop-Host

$hostExe = Join-Path $root ($IsWindows ? 'native/build/Release/megaprovider-host.exe' : 'native/build/megaprovider-host')
if ($Native -or -not (Test-Path $hostExe)) {
    $cmake = 'cmake'
    if ($IsWindows) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
        $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    }
    Push-Location (Join-Path $root 'native')
    try {
        if (-not (Test-Path 'build/CMakeCache.txt')) {
            & $cmake --preset ($IsWindows ? 'msvc' : 'linux')
            if ($LASTEXITCODE) { exit $LASTEXITCODE }
        }
        if ($IsWindows) { & $cmake --build --preset release -- -m '-v:minimal' } else { & $cmake --build --preset linux }
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    } finally { Pop-Location }
}

dotnet build (Join-Path $root 'MegaProvider.sln') -c $Configuration -nologo -v q
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$psd1 = Join-Path $root "src/MegaProvider/bin/$Configuration/net8.0/MegaProvider.psd1"
# In its own pwsh: whatever imports the module keeps its DLL locked.
pwsh -NoProfile -NonInteractive -File (Join-Path $PSScriptRoot 'Build-HelpFile.ps1') -ModulePath $psd1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($NoShell) { $psd1; return }
$backend = if ($Fake) { "`$env:MEGAPROVIDER_BACKEND = 'fake'; " } else { '' }
pwsh -NoLogo -NoExit -Command "$backend Import-Module '$psd1'; Set-Location mega:"
