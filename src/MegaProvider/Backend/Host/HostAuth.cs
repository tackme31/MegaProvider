using System.Runtime.Versioning;
using System.Text.Json.Nodes;

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

    public string Connect(string email, string password, string? authCode = null, Action<LoginProgress>? progress = null)
    {
        lock (_gate)
        {
            _verifiedAt = DateTime.MinValue;
            JsonNode result;
            try
            {
                result = client.Call("login", new { email, password, authCode }, Relay(progress))!;
            }
            catch (HostException e) when (e.Code == HostClient.CodeMfaRequired)
            {
                throw new MegaAuthCodeRequiredException();
            }
            // The API has no distinct code for a wrong 2FA code; these are the ones MegaExplorer saw for it.
            catch (HostException e) when (e.Code is HostClient.CodeNoEnt or HostClient.CodeFailed or HostClient.CodeExpired)
            {
                throw new UnauthorizedAccessException(authCode is null
                    ? "Wrong e-mail address or password."
                    : "Wrong e-mail address, password or authentication code.", e);
            }
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

    public void EnsureSession(Action<LoginProgress>? progress = null) => EnsureSession(force: false, progress);

    /// <summary>Makes the host hold our saved session, resuming it if the host has none or another one.</summary>
    public void EnsureSession(bool force, Action<LoginProgress>? progress = null)
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
                    client.Call("resume", new { session = ours }, Relay(progress));
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

    private static Action<JsonObject>? Relay(Action<LoginProgress>? progress) =>
        progress is null ? null : p =>
        {
            var stage = p["stage"]?.GetValue<string>() switch
            {
                "login" => LoginStage.LoggingIn,
                "load" => LoginStage.Loading,
                "download" => LoginStage.Downloading,
                "build" => LoginStage.Building,
                _ => (LoginStage?)null,
            };
            if (stage is not null)
                progress(new LoginProgress(stage.Value, p["done"]?.GetValue<long>() ?? 0, p["total"]?.GetValue<long>() ?? 0));
        };

    public void Invalidate()
    {
        lock (_gate) _verifiedAt = DateTime.MinValue;
    }
}
