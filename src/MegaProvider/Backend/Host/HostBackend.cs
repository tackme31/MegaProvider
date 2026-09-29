using System.Text.Json.Nodes;

namespace MegaProvider.Backend.Host;

/// <summary>
/// Everything is addressed by handle; the host keeps the node tree in memory, so a listing is
/// one sub-millisecond pipe round trip.
/// </summary>
internal sealed class HostBackend(HostClient client, HostAuth auth) : IMegaBackend
{
    private static readonly MegaItem Root = new("", "", true, 0, DateTime.MinValue, DateTime.MinValue, false, false, null);

    // PowerShell re-resolves each output item's path, which re-lists (and re-parses) every
    // ancestor: without this, `ls -Recurse` on a 700-item folder took 2.4 s. Short enough that
    // changes made elsewhere still show up at the next prompt; our own changes clear it.
    private static readonly TimeSpan CacheTtl = TimeSpan.FromSeconds(2);
    private readonly Dictionary<string, (DateTime FetchedAt, IReadOnlyList<MegaItem> Items)> _listings = new();
    private int _generation;

    public MegaItem? Get(string path) => ResolveChain(path)?[^1];

    public IReadOnlyList<MegaItem> List(string folderPath) => ListChildren(RequireFolder(folderPath));

    public IReadOnlyList<(string RelativePath, MegaItem Item)> Search(string folderPath, bool recurse, MegaCategory? category, bool favoritesOnly)
    {
        var chain = RequireChain(folderPath);
        if (!chain[^1].IsFolder) throw new InvalidOperationException($"'{folderPath}' is not a folder.");
        var results = Call("search", new
        {
            handle = HandleOrNull(chain[^1]),
            recursive = recurse,
            category = category?.ToString().ToLowerInvariant(),
            favourite = favoritesOnly,
        })!.AsArray();
        // A recursive search reports root-relative names; the folder's own part is dropped.
        return results.Select(n =>
        {
            var item = ToItem(n!);
            var path = recurse ? string.Join('/', n!["names"]!.AsArray().Skip(chain.Count - 1).Select(x => x!.GetValue<string>())) : item.Name;
            return (path, item);
        }).ToList();
    }

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

    public MegaItem Copy(string path, string destinationFolderPath, string newName)
    {
        CheckName(newName);
        var item = RequireChain(path)[^1];
        var dest = RequireFolder(destinationFolderPath);
        Call("copy", new { handle = item.Handle, parent = HandleOrNull(dest), name = newName == item.Name ? null : newName });
        // The host does not report the new handle; the caller made sure the name was free.
        return ListChildren(dest).FirstOrDefault(c => c.Name == newName && c.Handle != item.Handle)
               ?? throw new InvalidOperationException($"Copied '{newName}', but it is not in the destination folder.");
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

    public MegaLink? GetLink(string path)
    {
        var chain = RequireChain(path);
        var link = Call("link", new { handle = chain[^1].Handle });
        return link is null ? null : ToLink(PathOf(chain), chain[^1], link);
    }

    public IReadOnlyList<MegaLink> ListLinks() =>
        Call("links")!.AsArray().Select(n =>
        {
            var names = n!["names"]!.AsArray().Select(x => x!.GetValue<string>());
            return ToLink(string.Join('/', names), ToItem(n), n["link"]!);
        }).ToList();

    public MegaLink Publish(string path) => Export(path, null);

    public MegaLink Publish(string path, DateTime? expiresAt)
    {
        try
        {
            // 0 is MEGA's "never".
            return Export(path, expiresAt is { } at ? new DateTimeOffset(at).ToUnixTimeSeconds() : 0);
        }
        catch (HostException e) when (e.Code == HostClient.CodeAccess)
        {
            throw new MegaProPlanRequiredException("Link expiry dates");
        }
    }

    // A null expiry keeps the one the link has (the host passes it back to MEGA unchanged).
    private MegaLink Export(string path, long? expires)
    {
        var chain = RequireChain(path);
        return ToLink(PathOf(chain), chain[^1], Call("export", new { handle = chain[^1].Handle, expires })!);
    }

    public void Unpublish(string path) => Call("unexport", new { handle = RequireChain(path)[^1].Handle });

    public string ProtectLink(string url, string password) =>
        Call("protectLink", new { url, password })!["url"]!.GetValue<string>();

    public bool HasPaidPlan() => Call("plan")!["proLevel"]!.GetValue<int>() != 0;

    private static string PathOf(List<MegaItem> chain) => string.Join('/', chain.Skip(1).Select(i => i.Name));

    private static MegaLink ToLink(string path, MegaItem item, JsonNode link)
    {
        static DateTime? Time(JsonNode n) =>
            n.GetValue<long>() is > 0 and var t ? DateTimeOffset.FromUnixTimeSeconds(t).LocalDateTime : null;
        return new MegaLink(path, item.Handle, item.Name, item.IsFolder, link["url"]!.GetValue<string>(),
            Time(link["created"]!), Time(link["expires"]!), link["expired"]!.GetValue<bool>(), link["takenDown"]!.GetValue<bool>());
    }

    private JsonNode? Call(string op, object? args = null, Action<long, long>? progress = null)
    {
        if (op is not ("list" or "search" or "rubbish" or "path" or "restoreTarget" or "link" or "links" or "protectLink" or "plan"))
            lock (_listings) _listings.Clear();
        auth.EnsureSession();
        var relay = progress is null ? null : (Action<JsonObject>)(p => progress(p["done"]!.GetValue<long>(), p["total"]!.GetValue<long>()));
        try
        {
            return client.Call(op, args, relay);
        }
        catch (HostException e) when (e.Code is HostClient.CodeNotLoggedIn or HostClient.CodeBadSession)
        {
            // The host restarted, or its session was killed elsewhere; give it the saved one again.
            auth.Invalidate();
            auth.EnsureSession();
            return client.Call(op, args, relay);
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
        // Before the cache: a listing must not outlive Disconnect, nor show one account's files after Connect to another.
        auth.EnsureSession();
        lock (_listings)
        {
            if (_generation != auth.Generation)
            {
                _listings.Clear();
                _generation = auth.Generation;
            }
            if (_listings.TryGetValue(folder.Handle, out var cached) && DateTime.UtcNow - cached.FetchedAt < CacheTtl)
                return cached.Items;
        }
        var items = ToItems(Call("list", new { handle = HandleOrNull(folder) }));
        lock (_listings) _listings[folder.Handle] = (DateTime.UtcNow, items);
        return items;
    }

    private static IReadOnlyList<MegaItem> ToItems(JsonNode? list) => list!.AsArray().Select(n => ToItem(n!)).ToList();

    private static MegaItem ToItem(JsonNode n) => new(
        n["handle"]!.GetValue<string>(),
        n["name"]!.GetValue<string>(),
        n["folder"]!.GetValue<bool>(),
        n["size"]!.GetValue<long>(),
        DateTimeOffset.FromUnixTimeSeconds(n["mtime"]!.GetValue<long>()).LocalDateTime,
        DateTimeOffset.FromUnixTimeSeconds(n["ctime"]!.GetValue<long>()).LocalDateTime,
        n["favourite"]!.GetValue<bool>(),
        n["exported"]!.GetValue<bool>(),
        n["label"]!.GetValue<int>() is >= 1 and <= 7 and var label ? (MegaLabel)label : null);

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
            var next = NameMatch.Candidates(children, part, c => c.Name).FirstOrDefault();
            if (next is null) return null;
            chain.Add(next);
        }
        return chain;
    }
}
