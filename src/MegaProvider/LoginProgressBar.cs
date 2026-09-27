using System.Management.Automation;
using MegaProvider.Backend;

namespace MegaProvider;

/// <summary>Shows the steps of logging in (and loading the node list) as one PowerShell progress bar.</summary>
internal sealed class LoginProgressBar(Action<ProgressRecord> write)
{
    // Transfers use activity id 1; a login can happen in the middle of a Send-/Receive-MegaItem.
    private const int ActivityId = 2;
    private const string Activity = "Connecting to MEGA";
    private bool _shown;

    /// <summary>Makes sure the session is ready, showing progress if that means logging in; returns the backend.</summary>
    public static IMegaBackend EnsureSession(Action<ProgressRecord> write)
    {
        var backend = BackendHost.Backend;
        if (BackendHost.AuthOrNull is { } auth)
        {
            var bar = new LoginProgressBar(write);
            try
            {
                auth.EnsureSession(bar.Report);
            }
            // Only an early start for the progress bar. Not connected is for the actual call to report:
            // `cd mega:` needs no account (the root is answered without one), and an exception here
            // would surface as "path 'mega:\' does not exist".
            catch (MegaNotConnectedException)
            {
            }
            finally
            {
                bar.Complete();
            }
        }
        return backend;
    }

    public void Report(LoginProgress p)
    {
        var (status, percent) = p.Stage switch
        {
            LoginStage.LoggingIn => ("Logging in", -1),
            LoginStage.Loading => ("Loading the file list", -1),
            LoginStage.Downloading when p.Total > 0 =>
                ($"Downloading the file list ({Bytes.Format(p.Done)} / {Bytes.Format(p.Total)})", (int)Math.Min(100, p.Done * 100 / p.Total)),
            LoginStage.Downloading => ("Downloading the file list", -1),
            // No progress exists for this step, and on a large account it is the longest one.
            _ => ("Decrypting the file list (this can take a while on a large account)", -1),
        };
        _shown = true;
        write(new ProgressRecord(ActivityId, Activity, status) { PercentComplete = percent });
    }

    public void Complete()
    {
        if (!_shown) return;
        write(new ProgressRecord(ActivityId, Activity, "Done") { RecordType = ProgressRecordType.Completed });
    }
}

internal static class Bytes
{
    public static string Format(long n) =>
        n >= 1 << 30 ? $"{n / (double)(1 << 30):0.0} GB" : n >= 1 << 20 ? $"{n / (double)(1 << 20):0.0} MB" : $"{n / 1024.0:0} KB";
}
