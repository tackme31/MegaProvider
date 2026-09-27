namespace MegaProvider.Backend;

/// <summary>In-memory tree for trying the provider without a MEGA account.</summary>
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

    public MegaItem? Get(string path) => Resolve(path)?.ToItem();

    public IReadOnlyList<MegaItem> List(string folderPath) =>
        RequireFolder(folderPath).Children.Select(c => c.ToItem()).ToList();

    public void CreateFolder(string parentPath, string name) => Add(RequireFolder(parentPath), name, true);

    public void Rename(string path, string newName)
    {
        var n = Require(path);
        n.Name = newName;
        n.Modified = DateTime.Now;
    }

    public void Move(string path, string destinationFolderPath) => Reparent(Require(path), RequireFolder(destinationFolderPath));

    public void MoveToRubbish(string path) => Reparent(Require(path), _rubbish);

    private static void Reparent(Node n, Node newParent)
    {
        n.Parent?.Children.Remove(n);
        n.Parent = newParent;
        newParent.Children.Add(n);
    }
}
