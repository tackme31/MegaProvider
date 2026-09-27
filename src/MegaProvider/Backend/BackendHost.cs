using MegaProvider.Backend.MegaCmd;

namespace MegaProvider.Backend;

/// <summary>
/// The one place that picks the implementation. Swapping MEGAcmd for another backend means changing this file only.
/// Set MEGAPROVIDER_BACKEND=fake to use the in-memory tree without an account.
/// </summary>
internal static class BackendHost
{
    private static readonly Lazy<(IMegaBackend Backend, IMegaAuth? Auth)> Shared = new(Create);

    public static IMegaBackend Backend => Shared.Value.Backend;

    public static IMegaAuth Auth =>
        Shared.Value.Auth ?? throw new NotSupportedException("The fake backend has no account (MEGAPROVIDER_BACKEND=fake).");

    private static (IMegaBackend, IMegaAuth?) Create()
    {
        if (string.Equals(Environment.GetEnvironmentVariable("MEGAPROVIDER_BACKEND"), "fake", StringComparison.OrdinalIgnoreCase))
            return (new FakeBackend(), null);
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("The MEGAcmd backend currently supports Windows only.");
        var auth = new MegaCmdAuth();
        return (new MegaCmdBackend(auth), auth);
    }
}
