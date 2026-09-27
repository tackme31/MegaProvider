using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;

namespace MegaProvider.Backend;

/// <summary>
/// The session token, DPAPI-encrypted for the current user, as MegaExplorer does it.
/// Deliberately a different file from MegaExplorer's: the two projects never share a login.
/// </summary>
[SupportedOSPlatform("windows")]
internal static class SessionStore
{
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MegaProvider", "session.dat");

    // Not a secret (it ships in the DLL); it only stops a bare CryptUnprotectData from another same-user process.
    private static readonly byte[] Entropy = "MegaProvider.session.v1"u8.ToArray();

    public static string? Load()
    {
        if (!File.Exists(FilePath)) return null;
        var plain = ProtectedData.Unprotect(File.ReadAllBytes(FilePath), Entropy, DataProtectionScope.CurrentUser);
        return Encoding.UTF8.GetString(plain);
    }

    public static void Save(string token)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        var cipher = ProtectedData.Protect(Encoding.UTF8.GetBytes(token), Entropy, DataProtectionScope.CurrentUser);
        File.WriteAllBytes(FilePath, cipher);
    }

    public static void Clear()
    {
        if (File.Exists(FilePath)) File.Delete(FilePath);
    }
}
