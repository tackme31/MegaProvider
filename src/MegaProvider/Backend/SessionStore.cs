using System.Security.Cryptography;
using System.Text;

namespace MegaProvider.Backend;

/// <summary>
/// The session token of the current user. On Windows DPAPI-encrypted, as MegaExplorer (and MEGAsync)
/// do it. Elsewhere a plain file only the user can read, as MEGAcmd does it; MEGAsync only obfuscates
/// it there with a fixed key (docs/LINUX.md). Deliberately a different file from MegaExplorer's.
/// </summary>
internal static class SessionStore
{
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MegaProvider", "session.dat");

    // Not a secret (it ships in the DLL); it only stops a bare CryptUnprotectData from another same-user process.
    private static readonly byte[] Entropy = "MegaProvider.session.v1"u8.ToArray();

    public static string? Load()
    {
        if (!File.Exists(FilePath)) return null;
        var bytes = File.ReadAllBytes(FilePath);
        if (OperatingSystem.IsWindows())
            bytes = ProtectedData.Unprotect(bytes, Entropy, DataProtectionScope.CurrentUser);
        return Encoding.UTF8.GetString(bytes);
    }

    public static void Save(string token)
    {
        var bytes = Encoding.UTF8.GetBytes(token);
        if (OperatingSystem.IsWindows())
            bytes = ProtectedData.Protect(bytes, Entropy, DataProtectionScope.CurrentUser);
        PrivateFiles.WriteAllBytes(FilePath, bytes);
    }

    public static void Clear()
    {
        if (File.Exists(FilePath)) File.Delete(FilePath);
    }
}
