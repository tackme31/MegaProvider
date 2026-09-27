using MegaProvider.Backend.Host;
using MegaProvider.Backend.MegaCmd;

namespace MegaProvider.Backend;

/// <summary>
/// The one place that picks the implementation. MEGAPROVIDER_BACKEND selects it:
/// unset → the SDK host (native/), "megacmd" → MEGAcmd, "fake" → the in-memory tree (no account).
/// </summary>
internal static class BackendHost
{
    private static readonly Lazy<(IMegaBackend Backend, IMegaAuth? Auth)> Shared = new(Create);

    public static IMegaBackend Backend => Shared.Value.Backend;

    public static IMegaAuth Auth =>
        Shared.Value.Auth ?? throw new NotSupportedException("The fake backend has no account (MEGAPROVIDER_BACKEND=fake).");

    private static (IMegaBackend, IMegaAuth?) Create()
    {
        var choice = Environment.GetEnvironmentVariable("MEGAPROVIDER_BACKEND")?.ToLowerInvariant();
        if (choice == "fake")
            return (new FakeBackend(), null);
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("MegaProvider currently supports Windows only.");
        if (choice == "megacmd")
        {
            var cmdAuth = new MegaCmdAuth();
            return (new MegaCmdBackend(cmdAuth), cmdAuth);
        }
        var client = new HostClient();
        var auth = new HostAuth(client);
        return (new HostBackend(client, auth), auth);
    }
}
