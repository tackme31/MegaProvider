using MegaProvider.Backend.Host;

namespace MegaProvider.Backend;

/// <summary>
/// The one place that picks the implementation. MEGAPROVIDER_BACKEND selects it:
/// unset → the SDK host (native/), "fake" → the in-memory tree (no account).
/// </summary>
internal static class BackendHost
{
    private static readonly Lazy<(IMegaBackend Backend, IMegaAuth? Auth)> Shared = new(Create);

    public static IMegaBackend Backend => Shared.Value.Backend;

    /// <summary>Null for the fake backend, which has no account.</summary>
    public static IMegaAuth? AuthOrNull => Shared.Value.Auth;

    public static IMegaAuth Auth =>
        Shared.Value.Auth ?? throw new NotSupportedException("The fake backend has no account (MEGAPROVIDER_BACKEND=fake).");

    private static (IMegaBackend, IMegaAuth?) Create()
    {
        var choice = Environment.GetEnvironmentVariable("MEGAPROVIDER_BACKEND")?.ToLowerInvariant();
        if (choice == "fake")
            return (new FakeBackend(), null);
        // A leftover "megacmd" (that backend is gone) must not silently act on the real account.
        if (!string.IsNullOrEmpty(choice))
            throw new NotSupportedException($"Unknown MEGAPROVIDER_BACKEND '{choice}'. Leave it unset, or use 'fake'.");
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("MegaProvider currently supports Windows only.");
        var client = new HostClient();
        var auth = new HostAuth(client);
        return (new HostBackend(client, auth), auth);
    }
}
