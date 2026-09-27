using System.Globalization;
using System.Runtime.Versioning;
using System.Text.RegularExpressions;

namespace MegaProvider.Backend.MegaCmd;

/// <summary>
/// Read-only for now. Paths are resolved one level at a time by handle, never handed to MEGAcmd as-is:
/// MEGAcmd treats '*' and '?' in a path as wildcards, and a handle also sidesteps quoting non-ASCII names.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed partial class MegaCmdBackend(MegaCmdAuth auth) : IMegaBackend
{
    // The provider asks about the same path several times per cmdlet (exists? container? then list);
    // a short-lived cache turns that into one MEGAcmd call without hiding changes made elsewhere for long.
    private static readonly TimeSpan CacheTtl = TimeSpan.FromSeconds(10);

    private static readonly MegaItem Root = new("", "", true, 0, DateTime.MinValue);

    private readonly object _gate = new();
    private readonly Dictionary<string, (DateTime FetchedAt, IReadOnlyList<MegaItem> Items)> _listings = new();
    private int _cacheGeneration = -1;

    public MegaItem? Get(string path) => Resolve(path);

    public IReadOnlyList<MegaItem> List(string folderPath)
    {
        var folder = Resolve(folderPath) ?? throw new MegaItemNotFoundException($"'{folderPath}' not found.");
        return folder.IsFolder ? ListChildren(folder) : throw new InvalidOperationException($"'{folderPath}' is not a folder.");
    }

    public void CreateFolder(string parentPath, string name) => throw NotYet();
    public void Rename(string path, string newName) => throw NotYet();
    public void Move(string path, string destinationFolderPath) => throw NotYet();
    public void MoveToRubbish(string path) => throw NotYet();

    private static NotSupportedException NotYet() => new("Not implemented yet for the MEGAcmd backend.");

    private MegaItem? Resolve(string path)
    {
        var node = Root;
        foreach (var part in path.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            if (!node.IsFolder) return null;
            var children = ListChildren(node);
            // Same policy as FakeBackend: exact match first, then case-insensitive so `cd docs` works.
            var next = children.FirstOrDefault(c => c.Name == part)
                    ?? children.FirstOrDefault(c => string.Equals(c.Name, part, StringComparison.OrdinalIgnoreCase));
            if (next is null) return null;
            node = next;
        }
        return node;
    }

    private IReadOnlyList<MegaItem> ListChildren(MegaItem folder)
    {
        lock (_gate)
        {
            auth.EnsureSession();
            if (_cacheGeneration != auth.Generation)
            {
                _listings.Clear();
                _cacheGeneration = auth.Generation;
            }
            var key = folder.Handle == "" ? "/" : "H:" + folder.Handle;
            if (_listings.TryGetValue(key, out var cached) && DateTime.UtcNow - cached.FetchedAt < CacheTtl)
                return cached.Items;

            var items = Parse(RunLs(key));
            _listings[key] = (DateTime.UtcNow, items);
            return items;
        }
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
                DateTime.SpecifyKind(
                    DateTime.ParseExact(m.Groups["date"].Value, "yyyy-MM-ddTHH:mm:ss", CultureInfo.InvariantCulture),
                    DateTimeKind.Utc).ToLocalTime()));
        }
        return items;
    }

    [GeneratedRegex(@"^(?<flags>\S{4})\s+(?<vers>\S+)\s+(?<size>\S+)\s+(?<date>\S+)\s+H:(?<handle>\S+) (?<name>.*)$")]
    private static partial Regex Row();
}
