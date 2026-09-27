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
        public Node? Parent;
        public Node? RestoreParent;
        public List<Node> Children = new();

        public MegaItem ToItem() => new(Handle, Name, IsFolder, Size, Modified);
    }

    private readonly Node _root = new() { Handle = "root", Name = "", IsFolder = true };
    private readonly Node _rubbish = new() { Handle = "rubbish", Name = "Rubbish Bin", IsFolder = true };
    private int _nextHandle;

    public FakeBackend()
    {
        var docs = Add(_root, "docs", true);
        Add(docs, "readme.txt", false, 1200);
        Add(docs, "notes.txt", false, 340);
        var photos = Add(_root, "photos", true);
        var y2024 = Add(photos, "2024", true);
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
            // MEGA names are case-sensitive; the fallback is only so `cd Docs` feels like Windows.
            var next = node.Children.FirstOrDefault(c => c.Name == part)
                    ?? node.Children.FirstOrDefault(c => string.Equals(c.Name, part, StringComparison.OrdinalIgnoreCase));
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

    public MegaItem CreateFolder(string parentPath, string name) => Add(RequireFolder(parentPath), name, true).ToItem();

    public MegaItem Rename(string path, string newName)
    {
        var n = Require(path);
        n.Name = newName;
        n.Modified = DateTime.Now;
        return n.ToItem();
    }

    public MegaItem Move(string path, string destinationFolderPath)
    {
        var n = Require(path);
        Reparent(n, RequireFolder(destinationFolderPath));
        return n.ToItem();
    }

    public void MoveToRubbish(string path)
    {
        var n = Require(path);
        n.RestoreParent = n.Parent;
        Reparent(n, _rubbish);
    }

    public IReadOnlyList<MegaItem> ListRubbish() => _rubbish.Children.Select(c => c.ToItem()).ToList();

    public string Restore(string handle, string? destinationFolderPath)
    {
        var n = _rubbish.Children.FirstOrDefault(c => c.Handle == handle)
                ?? throw new MegaItemNotFoundException($"No item with handle '{handle}' in the Rubbish Bin.");
        var target = destinationFolderPath is not null
            ? RequireFolder(destinationFolderPath)
            : n.RestoreParent ?? throw new InvalidOperationException($"'{n.Name}' has no record of where it was removed from. Use -Destination.");
        Reparent(n, target);
        n.RestoreParent = null;
        return PathOf(n);
    }

    public MegaItem Upload(string localPath, string destinationFolderPath) =>
        throw new NotSupportedException("The fake backend does not simulate uploads.");

    public string Download(string path, string localFolder) =>
        throw new NotSupportedException("The fake backend does not simulate downloads.");

    private static void Reparent(Node n, Node newParent)
    {
        n.Parent?.Children.Remove(n);
        n.Parent = newParent;
        newParent.Children.Add(n);
    }
}
