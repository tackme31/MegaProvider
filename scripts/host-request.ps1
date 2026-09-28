# Sends requests straight to a running megaprovider-host and prints the replies, for debugging the
# protocol (docs/HOST.md) without the module. Session tokens in replies are masked.
#
#   ./scripts/host-request.ps1 status
#   ./scripts/host-request.ps1 list '{"handle":"xxxxxxxx"}'
#   ./scripts/host-request.ps1 list -Repeat 100          # prints the average round trip instead
#
# The host must already be running (any mega: command starts it). It is not started from here.
param(
    [Parameter(Mandatory)] [string]$Op,
    [string]$ArgsJson,
    [int]$Repeat = 1,
    [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
$name = & "$PSScriptRoot/Get-HostPipeName.ps1"
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $name, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
try { $pipe.Connect(2000) } catch [TimeoutException] { throw 'The host is not running. Run any mega: command first.' }
try {
    $writer = [IO.StreamWriter]::new($pipe); $writer.NewLine = "`n"
    $reader = [IO.StreamReader]::new($pipe)
    $request = if ($ArgsJson) { "{""id"":0,""op"":""$Op"",""args"":$ArgsJson}" } else { "{""id"":0,""op"":""$Op""}" }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    foreach ($i in 1..$Repeat) {
        $writer.WriteLine($request); $writer.Flush()
        do {
            $read = $reader.ReadLineAsync()
            if (-not $read.Wait([timespan]::FromSeconds($TimeoutSeconds))) { throw "No reply within $TimeoutSeconds s." }
            $line = $read.Result
            if ($Repeat -eq 1) { $line -replace '"session":"[^"]*"', '"session":"<masked>"' }
        } while ($line -match '"progress":')   # progress lines precede the final reply
    }
    if ($Repeat -gt 1) { '{0}: {1:0.00} ms per request' -f $Op, ($sw.Elapsed.TotalMilliseconds / $Repeat) }
} finally { $pipe.Dispose() }
