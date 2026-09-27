using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;

namespace MegaProvider.Backend.MegaCmd;

/// <summary>Runs MEGAclient.exe once per call. Facts about its behavior are in docs/MEGACMD.md.</summary>
internal static partial class MegaCmdClient
{
    public const int ExitNotFound = 53;
    public const int ExitNotLoggedIn = 57;

    private static readonly string ExePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MEGAcmd", "MEGAclient.exe");

    /// <summary>Returns stdout; throws <see cref="MegaCmdException"/> on a non-zero exit.</summary>
    public static string Run(params string[] args)
    {
        var (exitCode, stdout, stderr) = RunRaw(args);
        if (exitCode != 0)
            throw new MegaCmdException(exitCode, ErrorMessage(exitCode, stdout, stderr));
        return stdout;
    }

    public static (int ExitCode, string Stdout, string Stderr) RunRaw(params string[] args)
    {
        if (!File.Exists(ExePath))
            throw new InvalidOperationException($"MEGAcmd is not installed ('{ExePath}' not found).");

        var psi = new ProcessStartInfo(ExePath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args)
            psi.ArgumentList.Add(a);

        using var p = Process.Start(psi)!;
        // Some subcommands (e.g. `help <cmd>`) wait on a non-console stdin; never give them one.
        p.StandardInput.Close();
        var stderrTask = p.StandardError.ReadToEndAsync();
        var stdout = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return (p.ExitCode, stdout, stderrTask.Result);
    }

    // Arguments are never echoed into messages: `login` carries the password.
    private static string ErrorMessage(int exitCode, string stdout, string stderr)
    {
        var errors = ErrLine().Matches(stderr + "\n" + stdout).Select(m => m.Groups[1].Value.Trim()).ToList();
        return errors.Count > 0 ? string.Join(" ", errors) : $"MEGAcmd failed with exit code {exitCode}.";
    }

    [GeneratedRegex(@"^\[\S+ cmd ERR\s+(.*)\]\r?$", RegexOptions.Multiline)]
    private static partial Regex ErrLine();
}

public sealed class MegaCmdException(int exitCode, string message) : Exception(message)
{
    public int ExitCode { get; } = exitCode;
}
