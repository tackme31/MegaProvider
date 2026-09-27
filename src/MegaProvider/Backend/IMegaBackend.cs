namespace MegaProvider.Backend;

public sealed record MegaItem(string Handle, string Name, bool IsFolder, long Size, DateTime Modified);

/// <summary>
/// Paths are '/'-separated and relative to the cloud root ("" is the root).
/// MEGA allows same-named siblings, so a path can match several nodes; path-based calls take the first.
/// Mutating calls return the resulting item so the provider never has to look it up by (ambiguous) name.
/// </summary>
public interface IMegaBackend
{
    MegaItem? Get(string path);
    IReadOnlyList<MegaItem> List(string folderPath);
    MegaItem CreateFolder(string parentPath, string name);
    MegaItem Rename(string path, string newName);
    MegaItem Move(string path, string destinationFolderPath);

    /// <summary>Moves to the Rubbish Bin, remembering the parent so <see cref="Restore"/> can put it back.</summary>
    void MoveToRubbish(string path);

    /// <summary>Top-level items of the Rubbish Bin.</summary>
    IReadOnlyList<MegaItem> ListRubbish();

    /// <summary>
    /// Moves a Rubbish Bin item (by handle) back to where it was removed from, or to
    /// <paramref name="destinationFolderPath"/> when given. Returns the restored item's path.
    /// </summary>
    string Restore(string handle, string? destinationFolderPath);

    /// <summary>Uploads a local file or folder. An existing file of the same name gets a new version.</summary>
    MegaItem Upload(string localPath, string destinationFolderPath);

    /// <summary>Downloads into an existing local folder; returns the local path actually written.</summary>
    string Download(string path, string localFolder);
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
