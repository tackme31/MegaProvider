namespace MegaProvider.Backend;

public sealed record MegaItem(string Handle, string Name, bool IsFolder, long Size, DateTime Modified);

/// <summary>
/// Paths are '/'-separated and relative to the cloud root ("" is the root).
/// MEGA allows same-named siblings, so a path can match several nodes; path-based calls take the first.
/// </summary>
public interface IMegaBackend
{
    MegaItem? Get(string path);
    IReadOnlyList<MegaItem> List(string folderPath);
    void CreateFolder(string parentPath, string name);
    void Rename(string path, string newName);
    void Move(string path, string destinationFolderPath);
    void MoveToRubbish(string path);
}

/// <summary>Login state behind Connect-/Get-/Disconnect-MegaAccount. One account at a time.</summary>
public interface IMegaAuth
{
    /// <summary>Logs in, persists the session, and returns the account's e-mail.</summary>
    string Connect(string email, string password);

    /// <summary>The e-mail of the persisted session's account; throws <see cref="MegaNotConnectedException"/> if none.</summary>
    string GetAccountEmail();

    /// <summary>Invalidates the persisted session on the server and forgets it. No-op when not connected.</summary>
    void Disconnect();
}

public sealed class MegaItemNotFoundException(string message) : Exception(message);

public sealed class MegaNotConnectedException()
    : Exception("Not connected to MEGA. Run Connect-MegaAccount first.");
