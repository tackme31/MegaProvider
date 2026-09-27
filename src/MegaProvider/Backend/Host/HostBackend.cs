using System.Runtime.Versioning;
using System.Text.Json.Nodes;

namespace MegaProvider.Backend.Host;

/// <summary>
/// Everything is addressed by handle; the host keeps the node tree in memory, so a listing is
/// one sub-millisecond pipe round trip.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class HostBackend(HostClient client, HostAuth auth) : IMegaBackend
{
    private static readonly MegaItem Root = new("", "", true, 0, DateTime.MinValue);

    // PowerShell re-resolves each output item's path, which re-lists (and re-parses) every
    // ancestor: without this, `ls -Recurse` on a 700-item folder took 2.4 s. Short enough that
    // changes made elsewhere still show up at the next prompt; our own changes clear it.
    private static readonly TimeSpan CacheTtl = TimeSpan.FromSeconds(2);
    private readonly Dictionary<string, (DateTime FetchedAt, IReadOnlyList<MegaItem> Items)> _listings = new();

    public MegaItem? Get(string path) => ResolveChain(path)?[^1];

    public IReadOnlyList<MegaItem> List(string folderPath) => ListChildren(RequireFolder(folderPath));

    public MegaItem CreateFolder(string parentPath, string name)
    {
        CheckName(name);
        var parent = RequireFolder(parentPath);
        try
        {
            Call("mkdir", new { parent = HandleOrNull(parent), name });
        }
        catch (HostException e) when (e.Code == HostClient.CodeExist)
        {
            throw new InvalidOperationException($"A folder named '{name}' already exists there.", e);
        }
        // The server refuses a second folder of the same name, so this one is the new folder.
        return ListChildren(parent).First(c => c.IsFolder && c.Name == name);
    }

    public MegaItem Rename(string path, string newName)
    {
        CheckName(newName);
        var chain = RequireChain(path);
        Call("rename", new { handle = chain[^1].Handle, name = newName });
        return Reread(chain[^2], chain[^1].Handle);
    }

    public MegaItem Move(string path, string destinationFolderPath)
    {
        var item = RequireChain(path)[^1];
        var dest = RequireFolder(destinationFolderPath);
        Call("move", new { handle = item.Handle, parent = HandleOrNull(dest) });
        return Reread(dest, item.Handle);
    }

    public void MoveToRubbish(string path) => Call("trash", new { handle = RequireChain(path)[^1].Handle });

    public IReadOnlyList<MegaItem> ListRubbish() => ToItems(Call("rubbish"));

    public string Restore(string handle, string? destinationFolderPath)
    {
        if (ListRubbish().All(i => i.Handle != handle))
            throw new MegaItemNotFoundException($"No item with handle '{handle}' at the top of the Rubbish Bin.");
        // MEGA's own restore record (set by every client that bins a node, not just this one);
        // the host falls back to the root when the original folder is gone.
        var parent = destinationFolderPath is not null
            ? HandleOrNull(RequireFolder(destinationFolderPath))
            : Call("restoreTarget", new { handle })!["parent"]?.GetValue<string>();
        Call("move", new { handle, parent });
        var path = Call("path", new { handle })!;
        return string.Join('/', path["names"]!.AsArray().Select(n => n!.GetValue<string>()));
    }

    public MegaItem Upload(string localPath, string destinationFolderPath, Action<long, long>? progress = null)
    {
        var dest = RequireFolder(destinationFolderPath);
        var result = Call("upload", new { local = Path.GetFullPath(localPath), parent = HandleOrNull(dest) }, progress);
        var handle = result!["handle"]!.GetValue<string>();
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(localPath));
        var children = ListChildren(dest);
        return children.FirstOrDefault(c => c.Handle == handle)
               ?? children.Where(c => c.Name == name).MaxBy(c => c.Modified)
               ?? throw new InvalidOperationException($"Uploaded '{name}', but it is not in the destination folder.");
    }

    public string Download(string path, string localFolder, Action<long, long>? progress = null)
    {
        var item = RequireChain(path)[^1];
        // The SDK writes to this exact path, adding " (1)" when the name is taken; it reports what it used.
        var target = Path.Combine(Path.GetFullPath(localFolder), item.Name);
        return Call("download", new { handle = item.Handle, local = target }, progress)!["local"]!.GetValue<string>();
    }

    public void CancelTransfer() => client.Abort();

    private JsonNode? Call(string op, object? args = null, Action<long, long>? progress = null)
    {
        if (op is not ("list" or "rubbish" or "path" or "restoreTarget"))
            lock (_listings) _listings.Clear();
        auth.EnsureSession();
        try
        {
            return client.Call(op, args, progress);
        }
        catch (HostException e) when (e.Code is HostClient.CodeNotLoggedIn or HostClient.CodeBadSession)
        {
            // The host restarted, or its session was killed elsewhere; give it the saved one again.
            auth.Invalidate();
            auth.EnsureSession();
            return client.Call(op, args, progress);
        }
        catch (HostException e) when (e.Code == HostClient.CodeNoEnt)
        {
            throw new MegaItemNotFoundException("The item no longer exists (it may have been moved or deleted elsewhere).");
        }
    }

    private static string? HandleOrNull(MegaItem folder) => folder.Handle == "" ? null : folder.Handle;

    private static void CheckName(string name)
    {
        if (name.Length == 0 || name.Contains('/'))
            throw new ArgumentException($"'{name}' is not a valid MEGA name.");
    }

    private MegaItem Reread(MegaItem folder, string handle) =>
        ListChildren(folder).FirstOrDefault(c => c.Handle == handle)
        ?? throw new InvalidOperationException("The change was applied but the item could not be found afterwards.");

    private IReadOnlyList<MegaItem> ListChildren(MegaItem folder)
    {
        lock (_listings)
            if (_listings.TryGetValue(folder.Handle, out var cached) && DateTime.UtcNow - cached.FetchedAt < CacheTtl)
                return cached.Items;
        var items = ToItems(Call("list", new { handle = HandleOrNull(folder) }));
        lock (_listings) _listings[folder.Handle] = (DateTime.UtcNow, items);
        return items;
    }

    private static IReadOnlyList<MegaItem> ToItems(JsonNode? list) =>
        list!.AsArray().Select(n => new MegaItem(
            n!["handle"]!.GetValue<string>(),
            n["name"]!.GetValue<string>(),
            n["folder"]!.GetValue<bool>(),
            n["size"]!.GetValue<long>(),
            DateTimeOffset.FromUnixTimeSeconds(n["mtime"]!.GetValue<long>()).LocalDateTime)).ToList();

    private MegaItem RequireFolder(string path)
    {
        var item = RequireChain(path)[^1];
        return item.IsFolder ? item : throw new InvalidOperationException($"'{path}' is not a folder.");
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
            // Same policy as FakeBackend: exact match first, then case-insensitive so `cd docs` works.
            var next = children.FirstOrDefault(c => c.Name == part)
                    ?? children.FirstOrDefault(c => string.Equals(c.Name, part, StringComparison.OrdinalIgnoreCase));
            if (next is null) return null;
            chain.Add(next);
        }
        return chain;
    }
}
