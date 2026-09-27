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
