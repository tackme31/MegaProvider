using System.Collections.ObjectModel;
using System.Management.Automation;
using System.Management.Automation.Provider;
using MegaProvider.Backend;

namespace MegaProvider;

[CmdletProvider("Mega", ProviderCapabilities.ShouldProcess | ProviderCapabilities.Filter)]
public sealed class MegaCloudProvider : NavigationCmdletProvider
{
    // Providers are instantiated per call, so the backend (and its session) lives in BackendHost, not here.
    private static IMegaBackend Backend => BackendHost.Backend;

    protected override Collection<PSDriveInfo> InitializeDefaultDrives() =>
        new() { new PSDriveInfo("mega", ProviderInfo, "", "MEGA cloud drive", null) };

    // PowerShell hands paths over with '\' and, depending on the cmdlet, with or without a leading one.
    internal static string ToMegaPath(string path) =>
        path.Replace('\\', '/').Trim('/');

    private static string ToProviderPath(string megaPath) => megaPath.Replace('/', '\\');

    protected override bool IsValidPath(string path) => true;

    // The drive root is "", which the base implementation rejects; `cd mega:\; ls -Recurse` asks for it.
    protected override string GetChildName(string path) =>
        ToMegaPath(path).Length == 0 ? "" : base.GetChildName(path);

    protected override bool ItemExists(string path) => Backend.Get(ToMegaPath(path)) is not null;

    protected override bool IsItemContainer(string path) => Backend.Get(ToMegaPath(path))?.IsFolder == true;

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

    protected override void GetChildNames(string path, ReturnContainers returnContainers)
    {
        foreach (var child in Backend.List(ToMegaPath(path)))
            if (returnContainers == ReturnContainers.ReturnAllContainers && child.IsFolder || ShouldWrite(child))
                WriteItemObject(child.Name, MakePath(path, child.Name), child.IsFolder);
    }

    protected override void RenameItem(string path, string newName)
    {
        if (!ShouldProcess(path, $"Rename to '{newName}'")) return;
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

    protected override void NewItem(string path, string itemTypeName, object newItemValue)
    {
        if (!string.Equals(itemTypeName, "Directory", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(itemTypeName, "Folder", StringComparison.OrdinalIgnoreCase))
        {
            WriteError(new ErrorRecord(new NotSupportedException("Only -ItemType Directory is supported."),
                "NewItemTypeNotSupported", ErrorCategory.NotImplemented, path));
            return;
        }
        if (!ShouldProcess(path, "Create folder")) return;
        var created = Backend.CreateFolder(ToMegaPath(GetParentPath(path, "")), GetChildName(path));
        WriteItemObject(created, path, true);
    }

    // The destination must be an existing folder. MEGA keeps same-named siblings side by side, so
    // moving onto a name that already exists there adds a second item rather than replacing the first.
    protected override void MoveItem(string path, string destination)
    {
        if (!ShouldProcess(path, $"Move to '{destination}'")) return;
        var moved = Backend.Move(ToMegaPath(path), ToMegaPath(destination));
        WriteItemObject(moved, MakePath(ToProviderPath(ToMegaPath(destination)), moved.Name), moved.IsFolder);
    }

    // Remove-Item goes to the Rubbish Bin, never a permanent delete: there is no undo on this drive.
    // Restore-MegaItem puts it back.
    protected override void RemoveItem(string path, bool recurse)
    {
        if (!ShouldProcess(path, "Move to Rubbish Bin")) return;
        Backend.MoveToRubbish(ToMegaPath(path));
    }

    private static ErrorRecord NotFound(string path) =>
        new(new MegaItemNotFoundException($"'{path}' not found."), "ItemNotFound", ErrorCategory.ObjectNotFound, path);
}
