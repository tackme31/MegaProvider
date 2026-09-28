namespace MegaProvider.Backend;

/// <summary>
/// Folders and files only the current user can open. On Windows the per-user profile folders
/// already are; elsewhere the default umask leaves new ones readable by everybody.
/// </summary>
internal static class PrivateFiles
{
    private const UnixFileMode OwnerOnlyFolder = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private const UnixFileMode OwnerOnlyFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;

    public static void CreateDirectory(string path)
    {
        if (OperatingSystem.IsWindows())
        {
            Directory.CreateDirectory(path);
            return;
        }
        Directory.CreateDirectory(path, OwnerOnlyFolder);
        File.SetUnixFileMode(path, OwnerOnlyFolder); // also when it already existed
    }

    public static void WriteAllBytes(string path, byte[] bytes)
    {
        CreateDirectory(Path.GetDirectoryName(path)!);
        if (OperatingSystem.IsWindows())
        {
            File.WriteAllBytes(path, bytes);
            return;
        }
        // Created owner-only, so the content is never readable by others even for a moment.
        File.Delete(path);
        using var file = new FileStream(path, new FileStreamOptions
        {
            Mode = FileMode.CreateNew, Access = FileAccess.Write, UnixCreateMode = OwnerOnlyFile,
        });
        file.Write(bytes);
    }
}
