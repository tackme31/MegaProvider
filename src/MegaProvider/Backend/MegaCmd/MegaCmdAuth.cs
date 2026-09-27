using System.Runtime.Versioning;
using System.Text.RegularExpressions;

namespace MegaProvider.Backend.MegaCmd;

/// <summary>
/// Our session file is the source of truth, not MEGAcmd: its server is shared by every MEGAcmd user on
/// this Windows account and restores whatever session it last had. So before acting we check that the
/// session MEGAcmd holds is ours, and swap it in if not.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed partial class MegaCmdAuth : IMegaAuth
{
    private static readonly TimeSpan VerifyInterval = TimeSpan.FromSeconds(30);

    private readonly object _gate = new();
    private DateTime _verifiedAt = DateTime.MinValue;

    /// <summary>Bumped whenever the logged-in session may have changed; caches keyed to it must be dropped.</summary>
    public int Generation { get; private set; }

    // MEGAcmd prints nothing while it logs in, so there is no progress to relay.
    public string Connect(string email, string password, Action<LoginProgress>? progress = null)
    {
        lock (_gate)
        {
            Invalidate();
            if (CurrentToken() is not null)
                LeaveCurrentSession();
            MegaCmdClient.Run("login", email, password);
            SessionStore.Save(CurrentToken() ?? throw new InvalidOperationException("MEGAcmd reported no session after login."));
            _verifiedAt = DateTime.UtcNow;
            return WhoAmI();
        }
    }

    public string GetAccountEmail()
    {
        lock (_gate)
        {
            EnsureSession();
            return WhoAmI();
        }
    }

    public void Disconnect()
    {
        lock (_gate)
        {
            if (SessionStore.Load() is null) return;
            EnsureSession(force: true);
            MegaCmdClient.Run("logout");
            SessionStore.Clear();
            Invalidate();
        }
    }

    public void EnsureSession(Action<LoginProgress>? progress = null) => EnsureSession(force: false);

    /// <summary>Makes MEGAcmd hold our persisted session. Cheap when verified within the last few seconds.</summary>
    public void EnsureSession(bool force)
    {
        lock (_gate)
        {
            if (!force && DateTime.UtcNow - _verifiedAt < VerifyInterval) return;
            var ours = SessionStore.Load() ?? throw new MegaNotConnectedException();
            var current = CurrentToken();
            if (current != ours)
            {
                Invalidate();
                if (current is not null)
                    LeaveCurrentSession();
                MegaCmdClient.Run("login", ours);
            }
            _verifiedAt = DateTime.UtcNow;
        }
    }

    /// <summary>Forces the next <see cref="EnsureSession"/> to check again, e.g. after a "Not logged in" error.</summary>
    public void Invalidate()
    {
        _verifiedAt = DateTime.MinValue;
        Generation++;
    }

    // --keep-session: the session we step away from may be someone else's, and plain logout would kill it server-side.
    private static void LeaveCurrentSession() => MegaCmdClient.Run("logout", "--keep-session");

    private static string? CurrentToken()
    {
        var (exitCode, stdout, _) = MegaCmdClient.RunRaw("session");
        if (exitCode == MegaCmdClient.ExitNotLoggedIn) return null;
        if (exitCode != 0) throw new MegaCmdException(exitCode, $"MEGAcmd 'session' failed with exit code {exitCode}.");
        var m = SessionLine().Match(stdout);
        return m.Success ? m.Groups[1].Value : throw new InvalidOperationException("Unrecognized output from MEGAcmd 'session'.");
    }

    private static string WhoAmI()
    {
        var m = EmailLine().Match(MegaCmdClient.Run("whoami"));
        return m.Success ? m.Groups[1].Value : throw new InvalidOperationException("Unrecognized output from MEGAcmd 'whoami'.");
    }

    [GeneratedRegex(@"session is:\s*(\S+)")]
    private static partial Regex SessionLine();

    [GeneratedRegex(@"^Account e-mail:\s*(\S+)", RegexOptions.Multiline)]
    private static partial Regex EmailLine();
}
