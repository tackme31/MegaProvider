using System.Globalization;
using System.Runtime.Versioning;
using System.Text.RegularExpressions;

namespace MegaProvider.Backend.MegaCmd;

/// <summary>
/// Existing nodes are always addressed by handle ("H:xxxx"): MEGAcmd treats '*' and '?' in a path as
/// wildcards. A full path is used only where a new name has to be spelled out (rename, mkdir), because
/// "H:parent/newname" is silently ignored. Behavior this relies on is measured in docs/MEGACMD.md.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed partial class MegaCmdBackend(MegaCmdAuth auth) : IMegaBackend
{
    // The provider asks about the same path several times per cmdlet (exists? container? then list);
    // a short-lived cache turns that into one MEGAcmd call without hiding changes made elsewhere for long.
    private static readonly TimeSpan CacheTtl = TimeSpan.FromSeconds(10);

    // Custom node attribute holding the parent handle at removal time ("/" for the root, whose handle
    // `ls` never shows). MEGA's own restore attribute is not reachable through MEGAcmd.
    private const string RestoreAttr = "mp_rr";
    private const string RubbishPath = "//bin";

    private static readonly MegaItem Root = new("", "", true, 0, DateTime.MinValue);

    private readonly object _gate = new();
    private readonly Dictionary<string, (DateTime FetchedAt, IReadOnlyList<MegaItem> Items)> _listings = new();
    private int _cacheGeneration = -1;

    public MegaItem? Get(string path) => ResolveChain(path)?[^1];

    public IReadOnlyList<MegaItem> List(string folderPath) => ListChildren(RequireFolder(folderPath));

    public MegaItem CreateFolder(string parentPath, string name)
    {
        CheckName(name);
        var chain = RequireFolderChain(parentPath);
        Mutate(() => MegaCmdClient.Run("mkdir", AddressablePath(chain) + name));
        return FindChild(chain[^1], c => c.IsFolder && c.Name == name)
               ?? throw Unconfirmed($"create folder '{name}'");
    }

    public MegaItem Rename(string path, string newName)
    {
        CheckName(newName);
        var chain = RequireChain(path);
        var (item, parent) = (chain[^1], chain[^2]);
        if (item.Name == newName) return item;
        // `mv x existing-folder` moves x *into* that folder instead of renaming, and MEGAcmd's own
        // lookup of the target may ignore case, so any sibling that could be hit is refused.
        if (ListChildren(parent, fresh: true).Any(c => c.Handle != item.Handle
                && string.Equals(c.Name, newName, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException($"An item named '{newName}' already exists in that folder.");
        Mutate(() => MegaCmdClient.Run("mv", "H:" + item.Handle, AddressablePath(chain[..^1]) + newName));
        return FindChild(parent, c => c.Handle == item.Handle && c.Name == newName)
               ?? throw Unconfirmed($"rename '{item.Name}' to '{newName}'");
    }

    public MegaItem Move(string path, string destinationFolderPath)
    {
        var item = RequireChain(path)[^1];
        var dest = RequireFolder(destinationFolderPath);
        Mutate(() => MegaCmdClient.Run("mv", "H:" + item.Handle, FolderKey(dest)));
        return FindChild(dest, c => c.Handle == item.Handle) ?? throw Unconfirmed($"move '{item.Name}'");
    }

    public void MoveToRubbish(string path)
    {
        var chain = RequireChain(path);
        var (item, parent) = (chain[^1], chain[^2]);
        Mutate(() =>
        {
            MegaCmdClient.Run("attr", "H:" + item.Handle, "-s", RestoreAttr, parent.Handle == "" ? "/" : parent.Handle);
            MegaCmdClient.Run("mv", "H:" + item.Handle, RubbishPath);
        });
    }

    public IReadOnlyList<MegaItem> ListRubbish() => Fetch(RubbishPath, fresh: true);

    public string Restore(string handle, string? destinationFolderPath)
    {
        var item = Fetch(RubbishPath, fresh: true).FirstOrDefault(c => c.Handle == handle)
                   ?? throw new MegaItemNotFoundException($"No item with handle '{handle}' at the top of the Rubbish Bin.");
        string targetKey;
        if (destinationFolderPath is not null)
        {
            targetKey = FolderKey(RequireFolder(destinationFolderPath));
        }
        else
        {
            var recorded = RestoreAttrLine().Match(MegaCmdClient.Run("attr", "H:" + handle));
            if (!recorded.Success)
                throw new InvalidOperationException(
                    $"'{item.Name}' was not removed by this module, so where it came from is unknown. Use -Destination.");
            targetKey = recorded.Groups[1].Value == "/" ? "/" : "H:" + recorded.Groups[1].Value;
        }

        var targetPath = FolderPathOf(targetKey);
        if (targetPath.StartsWith(RubbishPath, StringComparison.Ordinal))
            throw new InvalidOperationException($"The folder '{item.Name}' was removed from is itself in the Rubbish Bin. Use -Destination.");
        Mutate(() => MegaCmdClient.Run("mv", "H:" + handle, targetKey));
        MegaCmdClient.RunRaw("attr", "H:" + handle, "-d", RestoreAttr); // best effort; a stale record is harmless
        return (targetPath.Trim('/') is var p && p.Length > 0 ? p + "/" : "") + item.Name;
    }

    public MegaItem Upload(string localPath, string destinationFolderPath, Action<long, long>? progress = null)
    {
        var dest = RequireFolder(destinationFolderPath);
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(localPath));
        Mutate(() => MegaCmdClient.Run("put", localPath, FolderKey(dest)));
        // A re-uploaded file becomes a new version with a new handle, so match by name, newest first.
        return ListChildren(dest).Where(c => c.Name == name).MaxBy(c => c.Modified)
               ?? throw Unconfirmed($"upload '{name}'");
    }

    public string Download(string path, string localFolder, Action<long, long>? progress = null)
    {
        var item = RequireChain(path)[^1];
        var output = MegaCmdClient.Run("get", "H:" + item.Handle, localFolder);
        // MEGAcmd appends " (N)" instead of overwriting a different local file, so take the path it reports.
        var finished = DownloadFinished().Matches(output);
        return finished.Count > 0 ? finished[^1].Groups[1].Value.Trim() : Path.Combine(localFolder, item.Name);
    }

    // MEGAcmd runs each transfer to completion in its own process; there is nothing to signal.
    public void CancelTransfer() { }

    private static void CheckName(string name)
    {
        if (name.Length == 0 || name.Contains('/'))
            throw new ArgumentException($"'{name}' is not a valid MEGA name.");
    }

    private static InvalidOperationException Unconfirmed(string what) =>
        new($"MEGAcmd reported success but the change could not be confirmed ({what}). Check the folder with Get-ChildItem.");

    // A full path to spell a new name under. Only the ancestors are in it; the new name itself is
    // never pattern-matched because it does not exist yet (measured with "star*q?.txt").
    private static string AddressablePath(IReadOnlyList<MegaItem> folderChain)
    {
        var path = "/" + string.Concat(folderChain.Skip(1).Select(n => n.Name + "/"));
        if (path.AsSpan().IndexOfAny('*', '?') >= 0)
            throw new NotSupportedException($"MEGAcmd would read '*' or '?' in '{path}' as a wildcard; cannot name items there yet.");
        return path;
    }

    private static string FolderKey(MegaItem folder) => folder.Handle == "" ? "/" : "H:" + folder.Handle;

    private void Mutate(Action action)
    {
        lock (_gate)
        {
            auth.EnsureSession();
            try
            {
                action();
            }
            finally
            {
                _listings.Clear();
            }
        }
    }

    private MegaItem? FindChild(MegaItem folder, Func<MegaItem, bool> match) =>
        ListChildren(folder, fresh: true).FirstOrDefault(match);

    private MegaItem RequireFolder(string path)
    {
        var item = RequireChain(path)[^1];
        return item.IsFolder ? item : throw new InvalidOperationException($"'{path}' is not a folder.");
    }

    private List<MegaItem> RequireFolderChain(string path)
    {
        var chain = RequireChain(path);
        return chain[^1].IsFolder ? chain : throw new InvalidOperationException($"'{path}' is not a folder.");
    }

    private List<MegaItem> RequireChain(string path) =>
        ResolveChain(path) ?? throw new MegaItemNotFoundException($"'{path}' not found.");

    /// <summary>The root followed by each node down to the target, or null if any step is missing.</summary>
    private List<MegaItem>? ResolveChain(string path)
    {
        var chain = new List<MegaItem> { Root };
        foreach (var part in path.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            var node = chain[^1];
            if (!node.IsFolder) return null;
            var children = ListChildren(node);
            var next = NameMatch.Candidates(children, part, c => c.Name).FirstOrDefault();
            if (next is null) return null;
            chain.Add(next);
        }
        return chain;
    }

    private IReadOnlyList<MegaItem> ListChildren(MegaItem folder, bool fresh = false) => Fetch(FolderKey(folder), fresh);

    private IReadOnlyList<MegaItem> Fetch(string key, bool fresh)
    {
        lock (_gate)
        {
            auth.EnsureSession();
            if (_cacheGeneration != auth.Generation)
            {
                _listings.Clear();
                _cacheGeneration = auth.Generation;
            }
            if (!fresh && _listings.TryGetValue(key, out var cached) && DateTime.UtcNow - cached.FetchedAt < CacheTtl)
                return cached.Items;

            var items = Parse(RunLs(key));
            _listings[key] = (DateTime.UtcNow, items);
            return items;
        }
    }

    // A handle-addressed `ls` starts with "/full/path: ", which is the only way to learn a folder's path from its handle.
    private string FolderPathOf(string key)
    {
        if (key == "/") return "/";
        var first = RunLs(key).Split('\n')[0].TrimEnd('\r', ' ');
        return first.EndsWith(':') ? first[..^1] : throw new InvalidOperationException($"Unrecognized output from MEGAcmd 'ls {key}'.");
    }

    private string RunLs(string key)
    {
        string[] args = ["ls", "-l", "--show-handles", "--time-format=ISO6081_WITH_TIME", key];
        try
        {
            return MegaCmdClient.Run(args);
        }
        catch (MegaCmdException e) when (e.ExitCode == MegaCmdClient.ExitNotLoggedIn)
        {
            // MEGAcmd lost the session since we last checked (someone ran logout, the server restarted...).
            auth.Invalidate();
            auth.EnsureSession();
            _cacheGeneration = auth.Generation;
            return MegaCmdClient.Run(args);
        }
        catch (MegaCmdException e) when (e.ExitCode == MegaCmdClient.ExitNotFound)
        {
            throw new MegaItemNotFoundException(key.StartsWith("H:", StringComparison.Ordinal)
                ? "The item no longer exists (it may have been moved or deleted elsewhere)."
                : e.Message);
        }
    }

    // Rows follow a "FLAGS VERS SIZE DATE HANDLE NAME" header; a handle-addressed listing also prints
    // "/full/path:" first. The name runs to the end of the line and may contain spaces.
    private static IReadOnlyList<MegaItem> Parse(string output)
    {
        var items = new List<MegaItem>();
        var inRows = false;
        foreach (var rawLine in output.Split('\n'))
        {
            var line = rawLine.TrimEnd('\r');
            if (!inRows)
            {
                inRows = line.StartsWith("FLAGS ", StringComparison.Ordinal);
                continue;
            }
            var m = Row().Match(line);
            if (!m.Success) continue;
            var type = m.Groups["flags"].Value[0];
            if (type != 'd' && type != '-') continue;
            var size = m.Groups["size"].Value;
            items.Add(new MegaItem(
                m.Groups["handle"].Value,
                m.Groups["name"].Value,
                type == 'd',
                size == "-" ? 0 : long.Parse(size, CultureInfo.InvariantCulture),
                // `ls --help` says UTC, but the dates are the machine's local time (measured).
                DateTime.SpecifyKind(
                    DateTime.ParseExact(m.Groups["date"].Value, "yyyy-MM-ddTHH:mm:ss", CultureInfo.InvariantCulture),
                    DateTimeKind.Local)));
        }
        return items;
    }

    [GeneratedRegex(@"^(?<flags>\S{4})\s+(?<vers>\S+)\s+(?<size>\S+)\s+(?<date>\S+)\s+H:(?<handle>\S+) (?<name>.*)$")]
    private static partial Regex Row();

    [GeneratedRegex(@"^\s*mp_rr = (\S+)", RegexOptions.Multiline)]
    private static partial Regex RestoreAttrLine();

    [GeneratedRegex(@"Download finished: ([^\r\n]+)")]
    private static partial Regex DownloadFinished();
}
