namespace MegaProvider.Backend;

/// <summary>In-memory tree for trying the provider without a MEGA account. Transfers are not simulated.</summary>
public sealed class FakeBackend : IMegaBackend
{
    private sealed class Node
    {
        public required string Handle;
        public required string Name;
        public required bool IsFolder;
        public long Size;
        public DateTime Modified = DateTime.Now;
        public DateTime Created = DateTime.Now;
        public MegaLabel? Label;
        public Node? Parent;
        public Node? RestoreParent;
        public bool IsFavorite;
        public DateTime? LinkCreated; // non-null while the node has a public link
        public DateTime? LinkExpires;
        public List<Node> Children = new();

        public MegaItem ToItem() => new(Handle, Name, IsFolder, Size, Modified, Created, IsFavorite, LinkCreated is not null, Label);
    }

    private readonly Node _root = new() { Handle = "root", Name = "", IsFolder = true };
    private readonly Node _rubbish = new() { Handle = "rubbish", Name = "Rubbish Bin", IsFolder = true };
    private int _nextHandle;

    public FakeBackend()
    {
        var docs = Add(_root, "docs", true);
        Add(docs, "readme.txt", false, 1200).IsFavorite = true;
        Add(docs, "notes.txt", false, 340).Label = MegaLabel.Red;
        var photos = Add(_root, "photos", true);
        var y2024 = Add(photos, "2024", true);
        y2024.IsFavorite = true;
        for (var i = 1; i <= 5; i++)
            Add(y2024, $"IMG_{i:0000}.jpg", false, 2_000_000 + i);
        Add(_root, "empty", true);
        // Two siblings with one name: legal on MEGA, and the case path-based addressing gets wrong.
        Add(_root, "dup.txt", false, 10);
        Add(_root, "dup.txt", false, 20);
    }

    private Node Add(Node parent, string name, bool isFolder, long size = 0)
    {
        var n = new Node { Handle = $"h{++_nextHandle:x6}", Name = name, IsFolder = isFolder, Size = size, Parent = parent };
        parent.Children.Add(n);
        return n;
    }

    private Node? Resolve(string path)
    {
        var node = _root;
        foreach (var part in path.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            var next = NameMatch.Candidates(node.Children, part, c => c.Name).FirstOrDefault();
            if (next is null) return null;
            node = next;
        }
        return node;
    }

    private Node Require(string path) =>
        Resolve(path) ?? throw new MegaItemNotFoundException($"'{path}' not found.");

    private Node RequireFolder(string path)
    {
        var n = Require(path);
        return n.IsFolder ? n : throw new InvalidOperationException($"'{path}' is not a folder.");
    }

    private static string PathOf(Node n) =>
        n.Parent is null ? "" : (PathOf(n.Parent) is var p && p.Length > 0 ? p + "/" : "") + n.Name;

    public MegaItem? Get(string path) => Resolve(path)?.ToItem();

    public IReadOnlyList<MegaItem> List(string folderPath) =>
        RequireFolder(folderPath).Children.Select(c => c.ToItem()).ToList();

    public IReadOnlyList<(string RelativePath, MegaItem Item)> Search(string folderPath, bool recurse, MegaCategory? category, bool favoritesOnly)
    {
        var folder = RequireFolder(folderPath);
        var prefix = PathOf(folder).Length;
        return (recurse ? Descendants(folder) : folder.Children)
            .Where(n => (!favoritesOnly || n.IsFavorite) && (category is null || !n.IsFolder && CategoryOf(n.Name) == category))
            .Select(n => (PathOf(n)[(prefix == 0 ? 0 : prefix + 1)..], n.ToItem()))
            .ToList();
    }

    // Roughly MEGA's mapping, enough for tests; the real one is the SDK's.
    private static MegaCategory CategoryOf(string name) => Path.GetExtension(name).ToLowerInvariant() switch
    {
        ".jpg" or ".jpeg" or ".png" or ".gif" or ".heic" or ".webp" => MegaCategory.Photo,
        ".mp3" or ".wav" or ".flac" or ".m4a" => MegaCategory.Audio,
        ".mp4" or ".mov" or ".mkv" or ".avi" => MegaCategory.Video,
        ".txt" or ".doc" or ".docx" or ".odt" or ".rtf" => MegaCategory.Document,
        ".pdf" => MegaCategory.Pdf,
        ".ppt" or ".pptx" or ".odp" => MegaCategory.Presentation,
        ".xls" or ".xlsx" or ".ods" or ".csv" => MegaCategory.Spreadsheet,
        ".zip" or ".rar" or ".7z" or ".gz" => MegaCategory.Archive,
        ".exe" or ".msi" or ".apk" => MegaCategory.Program,
        _ => MegaCategory.Other,
    };

    // MEGAPROVIDER_FAKE_EAGAIN=n[@k]: after k changes (counted from when the value was set), each change is
    // refused with EAGAIN n times before it goes through. Read on each call so a test can set it.
    // Exercises the same retry as the host, without the waits.
    private sealed class AgainException() : Exception("EAGAIN (simulated)");

    private static readonly TimeSpan[] NoDelays = RateLimit.DefaultDelays.Select(_ => TimeSpan.Zero).ToArray();

    private string? _eagainSetting;
    private int _changesSinceSetting;

    private T Change<T>(Func<T> apply)
    {
        var setting = Environment.GetEnvironmentVariable("MEGAPROVIDER_FAKE_EAGAIN");
        if (setting != _eagainSetting)
            (_eagainSetting, _changesSinceSetting) = (setting, 0);
        var parts = (setting ?? "0").Split('@');
        var refusals = _changesSinceSetting++ >= (parts.Length > 1 ? int.Parse(parts[1]) : 0) ? int.Parse(parts[0]) : 0;
        var attempt = 0;
        return RateLimit.Retry(() => attempt++ < refusals ? throw new AgainException() : apply(), e => e is AgainException, NoDelays);
    }

    public MegaItem CreateFolder(string parentPath, string name) =>
        Change(() => Add(RequireFolder(parentPath), name, true).ToItem());

    public MegaItem Rename(string path, string newName) => Change(() =>
    {
        var n = Require(path);
        n.Name = newName;
        n.Modified = DateTime.Now;
        return n.ToItem();
    });

    public MegaItem Move(string path, string destinationFolderPath) => Change(() =>
    {
        var n = Require(path);
        Reparent(n, RequireFolder(destinationFolderPath));
        return n.ToItem();
    });

    public MegaItem Copy(string path, string destinationFolderPath, string newName) => Change(() =>
    {
        // Clone before attaching, so copying a folder into itself cannot recurse forever.
        var copy = Clone(Require(path), null);
        copy.Name = newName;
        Reparent(copy, RequireFolder(destinationFolderPath));
        return copy.ToItem();
    });

    private Node Clone(Node n, Node? parent)
    {
        var c = new Node { Handle = $"h{++_nextHandle:x6}", Name = n.Name, IsFolder = n.IsFolder, Size = n.Size, Modified = n.Modified, Parent = parent };
        c.Children = n.Children.Select(child => Clone(child, c)).ToList();
        return c;
    }

    public void MoveToRubbish(string path) => Change(() =>
    {
        var n = Require(path);
        n.RestoreParent = n.Parent;
        Reparent(n, _rubbish);
        return true;
    });

    public IReadOnlyList<MegaItem> ListRubbish() => _rubbish.Children.Select(c => c.ToItem()).ToList();

    public string Restore(string handle, string? destinationFolderPath) => Change(() =>
    {
        var n = _rubbish.Children.FirstOrDefault(c => c.Handle == handle)
                ?? throw new MegaItemNotFoundException($"No item with handle '{handle}' in the Rubbish Bin.");
        var target = destinationFolderPath is not null
            ? RequireFolder(destinationFolderPath)
            : n.RestoreParent ?? throw new InvalidOperationException($"'{n.Name}' has no record of where it was removed from. Use -Destination.");
        Reparent(n, target);
        n.RestoreParent = null;
        return PathOf(n);
    });

    public MegaItem Upload(string localPath, string destinationFolderPath, Action<long, long>? progress = null) =>
        throw new NotSupportedException("The fake backend does not simulate uploads.");

    public string Download(string path, string localFolder, Action<long, long>? progress = null) =>
        throw new NotSupportedException("The fake backend does not simulate downloads.");

    public void CancelTransfer() { }

    public MegaLink? GetLink(string path) => Resolve(path) is { LinkCreated: not null } n ? LinkOf(n) : null;

    public IReadOnlyList<MegaLink> ListLinks() =>
        Descendants(_root).Where(n => n.LinkCreated is not null).Select(LinkOf).ToList();

    public MegaLink Publish(string path) => Change(() =>
    {
        var n = Require(path);
        n.LinkCreated ??= DateTime.Now;
        return LinkOf(n);
    });

    // MEGAPROVIDER_FAKE_PRO=1 makes the account a paid one. Like the server, the fake refuses an expiry
    // on a free account itself; a password it would compute anyway, as the SDK does.
    public MegaLink Publish(string path, DateTime? expiresAt) => Change(() =>
    {
        if (expiresAt is not null && !HasPaidPlan()) throw new MegaProPlanRequiredException("Link expiry dates");
        var n = Require(path);
        n.LinkCreated ??= DateTime.Now;
        n.LinkExpires = expiresAt;
        return LinkOf(n);
    });

    public void Unpublish(string path) => Change(() =>
    {
        var n = Require(path);
        (n.LinkCreated, n.LinkExpires) = (null, null);
        return true;
    });

    public string ProtectLink(string url, string password) =>
        "https://mega.nz/#P!" + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes($"{password}:{url}"));

    public bool HasPaidPlan() => Environment.GetEnvironmentVariable("MEGAPROVIDER_FAKE_PRO") == "1";

    private MegaLink LinkOf(Node n) => new(PathOf(n), n.Handle, n.Name, n.IsFolder,
        $"https://mega.nz/{(n.IsFolder ? "folder" : "file")}/{n.Handle}#key-{n.Handle}",
        n.LinkCreated, n.LinkExpires, n.LinkExpires < DateTime.Now, false);

    private static IEnumerable<Node> Descendants(Node n) => n.Children.SelectMany(c => Descendants(c).Prepend(c));

    private static void Reparent(Node n, Node newParent)
    {
        n.Parent?.Children.Remove(n);
        n.Parent = newParent;
        newParent.Children.Add(n);
    }
}
