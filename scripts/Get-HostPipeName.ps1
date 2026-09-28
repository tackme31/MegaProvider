# Prints where the module's host listens (HostClient.PipeName): a pipe name on Windows, a socket path
# elsewhere. Either goes straight into [IO.Pipes.NamedPipeClientStream]::new('.', <this>, ...).
if ($IsWindows) {
    'megaprovider-host-' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
} else {
    $dir = if ($env:XDG_RUNTIME_DIR) { Join-Path $env:XDG_RUNTIME_DIR 'megaprovider' }
    else { Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'MegaProvider/host' }
    Join-Path $dir 'host.sock'
}
