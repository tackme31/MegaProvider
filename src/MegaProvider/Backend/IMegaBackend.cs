namespace MegaProvider.Backend;

public sealed record MegaItem(string Handle, string Name, bool IsFolder, long Size, DateTime Modified);

/// <summary>
/// Paths are '/'-separated and relative to the cloud root ("" is the root).
/// MEGA allows same-named siblings, so a path can match several nodes; path-based calls take the first.
/// The provider refuses to change or transfer through such a path (MegaCloudProvider.EnsureUnambiguous).
/// Mutating calls return the resulting item so the provider never has to look it up by (ambiguous) name.
/// </summary>
public interface IMegaBackend
{
    MegaItem? Get(string path);
    IReadOnlyList<MegaItem> List(string folderPath);
    MegaItem CreateFolder(string parentPath, string name);
    MegaItem Rename(string path, string newName);
    MegaItem Move(string path, string destinationFolderPath);

    /// <summary>
    /// Copies a file, or a folder with everything in it, into the folder as <paramref name="newName"/>.
    /// If the folder already holds a file of that name, MEGA stacks the copy onto it as a new version
    /// instead of adding an item, so callers make sure the name is free first.
    /// </summary>
    MegaItem Copy(string path, string destinationFolderPath, string newName);

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
    /// <param name="progress">(bytes done, bytes total), called on the calling thread when supported.</param>
    MegaItem Upload(string localPath, string destinationFolderPath, Action<long, long>? progress = null);

    /// <summary>Downloads into an existing local folder; returns the local path actually written.</summary>
    string Download(string path, string localFolder, Action<long, long>? progress = null);

    /// <summary>
    /// Aborts the transfer running on another thread (Ctrl+C). The interrupted call then
    /// throws <see cref="OperationCanceledException"/>. No-op where transfers cannot be cancelled.
    /// </summary>
    void CancelTransfer();
}

public enum LoginStage { LoggingIn, Loading, Downloading, Building }

/// <summary>A step of logging in. Done/Total are bytes of the node list, known only while downloading.</summary>
public sealed record LoginProgress(LoginStage Stage, long Done = 0, long Total = 0);

/// <summary>Login state behind Connect-/Get-/Disconnect-MegaAccount. One account at a time.</summary>
public interface IMegaAuth
{
    /// <summary>Logs in, persists the session, and returns the account's e-mail.</summary>
    /// <param name="progress">Called on the calling thread when supported. Loading a large account takes minutes.</param>
    string Connect(string email, string password, Action<LoginProgress>? progress = null);

    /// <summary>
    /// Makes the backend hold the persisted session, logging in with it if needed (the slow part after a
    /// restart). Cheap when verified recently. Throws <see cref="MegaNotConnectedException"/> if there is none.
    /// </summary>
    void EnsureSession(Action<LoginProgress>? progress = null);

    /// <summary>The e-mail of the persisted session's account; throws <see cref="MegaNotConnectedException"/> if none.</summary>
    string GetAccountEmail();

    /// <summary>Invalidates the persisted session on the server and forgets it. No-op when not connected.</summary>
    void Disconnect();
}

/// <summary>How one path segment picks among a folder's children. Every backend resolves paths with this.</summary>
public static class NameMatch
{
    /// <summary>
    /// The children the name could mean: the exact matches, or, if there are none, the case-insensitive
    /// ones (MEGA names are case-sensitive; the fallback is only so `cd docs` feels like Windows).
    /// More than one means the name is ambiguous.
    /// </summary>
    public static List<T> Candidates<T>(IEnumerable<T> children, string name, Func<T, string> nameOf)
    {
        var all = children as IReadOnlyCollection<T> ?? children.ToList();
        var exact = all.Where(c => nameOf(c) == name).ToList();
        return exact.Count > 0 ? exact : all.Where(c => string.Equals(nameOf(c), name, StringComparison.OrdinalIgnoreCase)).ToList();
    }
}

public sealed class MegaItemNotFoundException(string message) : Exception(message);

public sealed class MegaAmbiguousPathException(string message) : Exception(message);

public sealed class MegaNotConnectedException()
    : Exception("Not connected to MEGA. Run Connect-MegaAccount first.");
