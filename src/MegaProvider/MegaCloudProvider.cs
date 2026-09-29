using System.Collections.ObjectModel;
using System.Management.Automation;
using System.Management.Automation.Provider;
using MegaProvider.Backend;

namespace MegaProvider;

[CmdletProvider("Mega", ProviderCapabilities.ShouldProcess | ProviderCapabilities.Filter)]
public sealed partial class MegaCloudProvider : NavigationCmdletProvider, IPropertyCmdletProvider
{
    // Providers are instantiated per call, so the backend (and its session) lives in BackendHost, not here.
    // Checking the session first lets a slow login (after the host restarted) show a progress bar.
    private IMegaBackend Backend => LoginProgressBar.EnsureSession(WriteProgress);

    protected override Collection<PSDriveInfo> InitializeDefaultDrives() =>
        new() { new PSDriveInfo("mega", ProviderInfo, "", "MEGA cloud drive", null) };

    // PowerShell's own separator for provider paths: it rewrites them to '/' outside Windows, so
    // handing back '\' there would leave paths half one way and half the other.
    private static readonly char Separator = OperatingSystem.IsWindows() ? '\\' : '/';

    // PowerShell hands paths over with '\' or '/' and, depending on the cmdlet, with or without a leading one.
    internal static string ToMegaPath(string path) =>
        path.Replace('\\', '/').Trim('/');

    private static string ToProviderPath(string megaPath) => megaPath.Replace('/', Separator);

    internal static string ToDrivePath(string megaPath) => "mega:" + Separator + ToProviderPath(megaPath);

    /// <summary>
    /// Throws if any segment of the path matches several same-named siblings. Backends silently take the
    /// first match, which is fine for looking but not for changing or transferring: a bulk rename could
    /// hit the wrong one. Missing segments pass; the operation itself reports them.
    /// </summary>
    internal static void EnsureUnambiguous(string megaPath)
    {
        var folder = "";
        foreach (var part in megaPath.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            var matches = NameMatch.Candidates(BackendHost.Backend.List(folder), part, c => c.Name);
            if (matches.Count > 1)
                throw new MegaAmbiguousPathException(
                    $"'{megaPath}' is ambiguous: {matches.Count} items named '{part}' are in the same folder. " +
                    "Rename one of them in another MEGA client first.");
            if (matches.Count == 0 || !matches[0].IsFolder) return;
            folder = folder.Length == 0 ? matches[0].Name : folder + "/" + matches[0].Name;
        }
    }

    private bool IsUnambiguous(params string[] paths)
    {
        foreach (var path in paths)
        {
            try
            {
                EnsureUnambiguous(ToMegaPath(path));
            }
            catch (MegaAmbiguousPathException e)
            {
                WriteError(new ErrorRecord(e, "AmbiguousPath", ErrorCategory.InvalidArgument, path));
                return false;
            }
        }
        return true;
    }

    protected override bool IsValidPath(string path) => true;

    // The drive root is "", which the base implementation rejects; `cd mega:\; ls -Recurse` asks for it.
    protected override string GetChildName(string path) =>
        ToMegaPath(path).Length == 0 ? "" : base.GetChildName(path);

    protected override bool ItemExists(string path) => Lookup(path) is not null;

    protected override bool IsItemContainer(string path) => Lookup(path)?.IsFolder == true;

    // PowerShell reads any exception from these checks as "path does not exist" (a terminating error
    // too), so the reason is written out first; its own "does not exist" still follows.
    private MegaItem? Lookup(string path)
    {
        try
        {
            return Backend.Get(ToMegaPath(path));
        }
        catch (MegaNotConnectedException e)
        {
            WriteError(new ErrorRecord(e, "NotConnected", ErrorCategory.AuthenticationError, path));
            return null;
        }
    }

    protected override bool HasChildItems(string path)
    {
        var item = Backend.Get(ToMegaPath(path));
        return item is { IsFolder: true } && Backend.List(ToMegaPath(path)).Count > 0;
    }

    protected override void GetItem(string path)
    {
        var item = Backend.Get(ToMegaPath(path));
        if (item is null)
        {
            WriteError(NotFound(path));
            return;
        }
        WriteItemObject(item, path, item.IsFolder);
    }

    public sealed class ChildItemsParameters
    {
        [Parameter] public SwitchParameter File { get; set; }
        [Parameter] public SwitchParameter Directory { get; set; }

        /// <summary>Only files of this kind, as MEGA classifies them by extension.</summary>
        [Parameter] public MegaCategory? Category { get; set; }

        /// <summary>Only items marked as favourites.</summary>
        [Parameter] public SwitchParameter Favorite { get; set; }

        internal bool NarrowsByIndex => Category is not null || Favorite;
    }

    protected override object GetChildItemsDynamicParameters(string path, bool recurse) => new ChildItemsParameters();

    protected override object GetChildNamesDynamicParameters(string path) => new ChildItemsParameters();

    // Declaring ProviderCapabilities.Filter makes -Filter this provider's job; PowerShell won't apply it.
    // -File / -Directory only decide what is written; -Recurse still descends into every folder.
    private bool ShouldWrite(MegaItem item)
    {
        if (DynamicParameters is ChildItemsParameters p)
        {
            if (p.File && item.IsFolder) return false;
            if (p.Directory && !item.IsFolder) return false;
        }
        return string.IsNullOrEmpty(Filter) || new WildcardPattern(Filter, WildcardOptions.IgnoreCase).IsMatch(item.Name);
    }

    protected override void GetChildItems(string path, bool recurse)
    {
        // Category and favourites are MEGA's own index: one search, not a walk of every folder.
        if (DynamicParameters is ChildItemsParameters { NarrowsByIndex: true } p)
        {
            foreach (var (relative, item) in Backend.Search(ToMegaPath(path), recurse, p.Category, p.Favorite))
            {
                if (Stopping) return;
                if (ShouldWrite(item))
                    WriteItemObject(item, MakePath(path, ToProviderPath(relative)), item.IsFolder);
            }
            return;
        }
        foreach (var child in Backend.List(ToMegaPath(path)))
        {
            if (Stopping) return;
            var childPath = MakePath(path, child.Name);
            if (ShouldWrite(child))
                WriteItemObject(child, childPath, child.IsFolder);
            if (recurse && child.IsFolder)
                GetChildItems(childPath, recurse);
        }
    }

    // With -Recurse, PowerShell calls this per folder: once for the names to show, once with
    // ReturnAllContainers for the folders to descend into, which must not be narrowed.
    protected override void GetChildNames(string path, ReturnContainers returnContainers)
    {
        var matching = DynamicParameters is ChildItemsParameters { NarrowsByIndex: true } p
            ? Backend.Search(ToMegaPath(path), false, p.Category, p.Favorite).Select(r => r.Item.Handle).ToHashSet()
            : null;
        foreach (var child in Backend.List(ToMegaPath(path)))
            if (returnContainers == ReturnContainers.ReturnAllContainers && child.IsFolder
                || ShouldWrite(child) && (matching is null || matching.Contains(child.Handle)))
                WriteItemObject(child.Name, MakePath(path, child.Name), child.IsFolder);
    }

    protected override void RenameItem(string path, string newName) => StopIfRateLimited(path, () => Rename(path, newName));

    private void Rename(string path, string newName)
    {
        if (!IsUnambiguous(path) || !ShouldProcess(path, $"Rename to '{newName}'")) return;
        // MEGA would accept a duplicate name, but in a bulk rename that is almost always two
        // inputs mapping to one output, so it is refused as Windows would.
        var parent = ToMegaPath(GetParentPath(path, ""));
        var self = Backend.Get(ToMegaPath(path));
        if (Backend.List(parent).Any(c => c.Name == newName && c.Handle != self?.Handle))
        {
            WriteError(new ErrorRecord(new InvalidOperationException($"An item named '{newName}' already exists in that folder."),
                "RenameTargetExists", ErrorCategory.ResourceExists, path));
            return;
        }
        var renamed = Backend.Rename(ToMegaPath(path), newName);
        WriteItemObject(renamed, MakePath(GetParentPath(path, ""), renamed.Name), renamed.IsFolder);
    }

    protected override void NewItem(string path, string itemTypeName, object newItemValue) =>
        StopIfRateLimited(path, () => NewFolder(path, itemTypeName));

    private void NewFolder(string path, string itemTypeName)
    {
        if (!string.Equals(itemTypeName, "Directory", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(itemTypeName, "Folder", StringComparison.OrdinalIgnoreCase))
        {
            WriteError(new ErrorRecord(new NotSupportedException("Only -ItemType Directory is supported."),
                "NewItemTypeNotSupported", ErrorCategory.NotImplemented, path));
            return;
        }
        if (!IsUnambiguous(GetParentPath(path, "")) || !ShouldProcess(path, "Create folder")) return;
        var created = Backend.CreateFolder(ToMegaPath(GetParentPath(path, "")), GetChildName(path));
        WriteItemObject(created, path, true);
    }

    // The destination must be an existing folder. MEGA would add a same-named sibling rather than
    // replace anything, so a name that already exists there is refused, as for Rename-Item.
    protected override void MoveItem(string path, string destination) => StopIfRateLimited(path, () => Move(path, destination));

    private void Move(string path, string destination)
    {
        if (!IsUnambiguous(path, destination)) return;
        var self = Backend.Get(ToMegaPath(path));
        if (self is not null && Backend.Get(ToMegaPath(destination)) is { IsFolder: true }
            && Backend.List(ToMegaPath(destination)).Any(c => c.Name == self.Name && c.Handle != self.Handle))
        {
            WriteError(new ErrorRecord(new InvalidOperationException($"An item named '{self.Name}' already exists in '{destination}'."),
                "MoveTargetExists", ErrorCategory.ResourceExists, path));
            return;
        }
        if (!ShouldProcess(path, $"Move to '{destination}'")) return;
        var moved = Backend.Move(ToMegaPath(path), ToMegaPath(destination));
        WriteItemObject(moved, MakePath(ToProviderPath(ToMegaPath(destination)), moved.Name), moved.IsFolder);
    }

    // A destination that is an existing folder receives the item under its own name; any other
    // destination names the copy. A name already taken there is refused: MEGA would stack a file
    // onto it as a version (or drop an identical one silently) and would add a same-named folder.
    protected override void CopyItem(string path, string copyPath, bool recurse) =>
        StopIfRateLimited(path, () => Copy(path, copyPath, recurse));

    private void Copy(string path, string copyPath, bool recurse)
    {
        if (!IsUnambiguous(path, copyPath)) return;
        var source = Backend.Get(ToMegaPath(path));
        if (source is null)
        {
            WriteError(NotFound(path));
            return;
        }
        if (source.IsFolder && !recurse)
        {
            WriteError(new ErrorRecord(new InvalidOperationException(
                    $"'{path}' is a folder. MEGA copies a folder with everything in it; use -Recurse to confirm."),
                "CopyFolderNeedsRecurse", ErrorCategory.InvalidOperation, path));
            return;
        }
        string destFolder, name;
        if (Backend.Get(ToMegaPath(copyPath)) is { IsFolder: true })
            (destFolder, name) = (copyPath, source.Name);
        else
            (destFolder, name) = (GetParentPath(copyPath, ""), GetChildName(copyPath));
        if (Backend.Get(ToMegaPath(destFolder)) is not { IsFolder: true })
        {
            WriteError(NotFound(destFolder));
            return;
        }
        if (Backend.List(ToMegaPath(destFolder)).Any(c => c.Name == name))
        {
            WriteError(new ErrorRecord(new InvalidOperationException($"An item named '{name}' already exists in '{destFolder}'."),
                "CopyTargetExists", ErrorCategory.ResourceExists, path));
            return;
        }
        if (!ShouldProcess(path, $"Copy to '{MakePath(destFolder, name)}'")) return;
        var copied = Backend.Copy(ToMegaPath(path), ToMegaPath(destFolder), name);
        WriteItemObject(copied, MakePath(ToProviderPath(ToMegaPath(destFolder)), copied.Name), copied.IsFolder);
    }

    // Remove-Item goes to the Rubbish Bin, never a permanent delete: there is no undo on this drive.
    // Restore-MegaItem puts it back.
    protected override void RemoveItem(string path, bool recurse) => StopIfRateLimited(path, () =>
    {
        if (!IsUnambiguous(path) || !ShouldProcess(path, "Move to Rubbish Bin")) return;
        Backend.MoveToRubbish(ToMegaPath(path));
    });

    // Any other failure is per item and the pipeline goes on. This one stops it: MEGA would refuse the
    // next items too, and stopping leaves the done ones as a prefix of the input to resume after.
    private void StopIfRateLimited(string path, Action change)
    {
        try
        {
            change();
        }
        catch (MegaRateLimitedException e)
        {
            ThrowTerminatingError(new ErrorRecord(e.StoppedAt(path), "RateLimited", ErrorCategory.LimitsExceeded, path));
        }
    }

    private static ErrorRecord NotFound(string path) =>
        new(new MegaItemNotFoundException($"'{path}' not found."), "ItemNotFound", ErrorCategory.ObjectNotFound, path);
}
