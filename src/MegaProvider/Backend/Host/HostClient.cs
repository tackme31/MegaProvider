using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.Versioning;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MegaProvider.Backend.Host;

/// <summary>
/// Talks to megaprovider-host.exe over a per-user named pipe, starting it when nobody answers.
/// One JSON object per line each way; the protocol is in docs/HOST.md.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class HostClient
{
    public const int CodeNotLoggedIn = 2;
    public const int CodeBadSession = -15;
    public const int CodeNoEnt = -9;
    public const int CodeAgain = -3;
    public const int CodeFailed = -5;
    public const int CodeExpired = -8;
    public const int CodeExist = -12;
    public const int CodeIncomplete = -13;
    public const int CodeMfaRequired = -26;

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(20);

    private static readonly string ExePath = Path.Combine(
        Path.GetDirectoryName(typeof(HostClient).Assembly.Location)!, "megaprovider-host.exe");

    private static readonly string DataDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MegaProvider", "host");

    // Per user: the host serves exactly one Windows account, and its pipe admits only that user.
    public static readonly string PipeName = "megaprovider-host-" + WindowsIdentity.GetCurrent().User!.Value;

    private readonly object _gate = new();
    private NamedPipeClientStream? _pipe;
    private StreamReader? _reader;
    private StreamWriter? _writer;
    private int _nextId;
    private volatile bool _aborted;

    /// <summary>Sends one request and waits for its result, relaying progress lines if any.</summary>
    public JsonNode? Call(string op, object? args = null, Action<JsonObject>? progress = null)
    {
        lock (_gate)
        {
            // EAGAIN is the API asking to back off; the request was not applied, so it is safe to repeat.
            for (var attempt = 0; ; attempt++)
            {
                try
                {
                    return CallOnce(op, args, progress);
                }
                catch (HostException e) when (e.Code == CodeAgain && attempt < 4)
                {
                    Thread.Sleep(TimeSpan.FromMilliseconds(500 * (1 << attempt)));
                }
            }
        }
    }

    /// <summary>
    /// Breaks the connection from another thread. The host notices on its next progress
    /// write and cancels the transfer; the blocked <see cref="Call"/> throws OperationCanceledException.
    /// </summary>
    public void Abort()
    {
        _aborted = true;
        _pipe?.Dispose();
    }

    private JsonNode? CallOnce(string op, object? args, Action<JsonObject>? progress)
    {
        _aborted = false;
        var id = ++_nextId;
        var request = new JsonObject { ["id"] = id, ["op"] = op };
        if (args is not null)
            request["args"] = JsonSerializer.SerializeToNode(args);
        Send(request.ToJsonString());
        try
        {
            for (;;)
            {
                var line = _reader!.ReadLine() ?? throw new IOException("The MEGA host closed the connection.");
                var message = JsonNode.Parse(line)!.AsObject();
                if (message["id"]?.GetValue<int>() != id) continue;
                if (message["progress"] is JsonObject p)
                {
                    progress?.Invoke(p);
                    continue;
                }
                if (message["ok"]!.GetValue<bool>())
                    return message["result"];
                var error = message["error"]!;
                throw new HostException(error["code"]!.GetValue<int>(), error["message"]!.GetValue<string>());
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
            Disconnect();
            if (_aborted) throw new OperationCanceledException("The transfer was cancelled.", e);
            // Not retried: the host may have applied the request before it went away.
            throw new IOException($"The MEGA host went away while handling '{op}'. Check the result before retrying.", e);
        }
    }

    // A connection kept from an earlier call is found dead only when written to (the host
    // exited idle, crashed, or was stopped for a rebuild). Nothing was delivered then, so
    // reconnecting — which restarts the host — and sending again is safe.
    private void Send(string line)
    {
        for (var attempt = 0; ; attempt++)
        {
            EnsureConnected();
            try
            {
                _writer!.WriteLine(line);
                _writer.Flush();
                return;
            }
            catch (IOException) when (attempt == 0)
            {
                Disconnect();
            }
        }
    }

    private void EnsureConnected()
    {
        if (_pipe is { IsConnected: true }) return;
        Disconnect();
        var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut);
        try
        {
            pipe.Connect(100);
        }
        catch (TimeoutException)
        {
            StartHost();
            var deadline = DateTime.UtcNow + StartTimeout;
            while (true)
            {
                try
                {
                    pipe.Connect(250);
                    break;
                }
                catch (TimeoutException) when (DateTime.UtcNow < deadline) { }
            }
        }
        _pipe = pipe;
        _reader = new StreamReader(pipe, new UTF8Encoding(false));
        _writer = new StreamWriter(pipe, new UTF8Encoding(false)) { NewLine = "\n" };
    }

    private void Disconnect()
    {
        _pipe?.Dispose();
        _pipe = null;
        _reader = null;
        _writer = null;
    }

    private static void StartHost()
    {
        if (!File.Exists(ExePath))
            throw new InvalidOperationException($"The MEGA host is missing ('{ExePath}'). Build native/ first (scripts/dev.ps1).");
        Directory.CreateDirectory(DataDir);
        // Through the shell so it inherits none of our handles: with UseShellExecute=false it would
        // hold this pwsh's redirected stdout open for its whole life, and `pwsh ... | sed` would never end.
        var psi = new ProcessStartInfo(ExePath) { UseShellExecute = true, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var a in new[] { "--pipe", PipeName, "--data", DataDir })
            psi.ArgumentList.Add(a);
        // Not waited on or killed: it outlives this PowerShell so the next one finds the nodes already loaded.
        using var _ = Process.Start(psi);
    }
}

public sealed class HostException(int code, string message) : Exception(message)
{
    public int Code { get; } = code;
}
