using System.Runtime.Versioning;

namespace MegaProvider.Backend.Host;

/// <summary>
/// The session file stays the source of truth: the host keeps its login only in memory,
/// so after it restarts (idle exit, crash, rebuild) we hand it the saved session again.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class HostAuth(HostClient client) : IMegaAuth
{
    // Checking costs one pipe round trip, so this only spares a burst of provider calls.
    private static readonly TimeSpan VerifyInterval = TimeSpan.FromSeconds(5);

    private readonly object _gate = new();
    private DateTime _verifiedAt = DateTime.MinValue;

    public string Connect(string email, string password)
    {
        lock (_gate)
        {
            _verifiedAt = DateTime.MinValue;
            var result = client.Call("login", new { email, password })!;
            SessionStore.Save(result["session"]!.GetValue<string>());
            _verifiedAt = DateTime.UtcNow;
            return result["email"]!.GetValue<string>();
        }
    }

    public string GetAccountEmail()
    {
        lock (_gate)
        {
            EnsureSession(force: true);
            return client.Call("status")!["email"]!.GetValue<string>();
        }
    }

    public void Disconnect()
    {
        lock (_gate)
        {
            if (SessionStore.Load() is null) return;
            EnsureSession(force: true);
            client.Call("logout");
            SessionStore.Clear();
            _verifiedAt = DateTime.MinValue;
        }
    }

    /// <summary>Makes the host hold our saved session, resuming it if the host has none or another one.</summary>
    public void EnsureSession(bool force = false)
    {
        lock (_gate)
        {
            if (!force && DateTime.UtcNow - _verifiedAt < VerifyInterval) return;
            var ours = SessionStore.Load() ?? throw new MegaNotConnectedException();
            var status = client.Call("status")!;
            if (!(status["loggedIn"]!.GetValue<bool>() && status["session"]?.GetValue<string>() == ours))
            {
                try
                {
                    client.Call("resume", new { session = ours });
                }
                catch (HostException e) when (e.Code == HostClient.CodeBadSession)
                {
                    // Revoked elsewhere (logout on another client, password change): only a new login helps.
                    throw new MegaNotConnectedException();
                }
            }
            _verifiedAt = DateTime.UtcNow;
        }
    }

    public void Invalidate()
    {
        lock (_gate) _verifiedAt = DateTime.MinValue;
    }
}
