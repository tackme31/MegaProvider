using System.Collections.ObjectModel;
using System.Management.Automation;
using System.Management.Automation.Provider;
using MegaProvider.Backend;

namespace MegaProvider;

[CmdletProvider("Mega", ProviderCapabilities.ShouldProcess | ProviderCapabilities.Filter)]
public sealed class MegaCloudProvider : NavigationCmdletProvider
{
    // One backend per process: providers are instantiated per call, so state must not live on the instance.
    private static readonly Lazy<IMegaBackend> SharedBackend = new(() => new FakeBackend());

    private static IMegaBackend Backend => SharedBackend.Value;

    protected override Collection<PSDriveInfo> InitializeDefaultDrives() =>
        new() { new PSDriveInfo("mega", ProviderInfo, "", "MEGA cloud drive", null) };

    // PowerShell hands paths over with '\' and, depending on the cmdlet, with or without a leading one.
    private static string ToMegaPath(string path) =>
        path.Replace('\\', '/').Trim('/');

    protected override bool IsValidPath(string path) => true;

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

    // Declaring ProviderCapabilities.Filter makes -Filter this provider's job; PowerShell won't apply it.
    private bool MatchesFilter(string name) =>
        string.IsNullOrEmpty(Filter) || new WildcardPattern(Filter, WildcardOptions.IgnoreCase).IsMatch(name);

    protected override void GetChildItems(string path, bool recurse)
    {
        foreach (var child in Backend.List(ToMegaPath(path)))
        {
            if (Stopping) return;
            var childPath = MakePath(path, child.Name);
            if (MatchesFilter(child.Name))
                WriteItemObject(child, childPath, child.IsFolder);
            if (recurse && child.IsFolder)
                GetChildItems(childPath, recurse);
        }
    }

    protected override void GetChildNames(string path, ReturnContainers returnContainers)
    {
        foreach (var child in Backend.List(ToMegaPath(path)))
            if (returnContainers == ReturnContainers.ReturnAllContainers && child.IsFolder || MatchesFilter(child.Name))
                WriteItemObject(child.Name, MakePath(path, child.Name), child.IsFolder);
    }

    protected override void RenameItem(string path, string newName)
    {
        if (!ShouldProcess(path, $"Rename to '{newName}'")) return;
        Backend.Rename(ToMegaPath(path), newName);
        var newPath = MakePath(GetParentPath(path, ""), newName);
        var renamed = Backend.Get(ToMegaPath(newPath))!;
        WriteItemObject(renamed, newPath, renamed.IsFolder);
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
        Backend.CreateFolder(ToMegaPath(GetParentPath(path, "")), GetChildName(path));
        WriteItemObject(Backend.Get(ToMegaPath(path))!, path, true);
    }

    protected override void MoveItem(string path, string destination)
    {
        if (!ShouldProcess(path, $"Move to '{destination}'")) return;
        Backend.Move(ToMegaPath(path), ToMegaPath(destination));
    }

    // Remove-Item goes to the Rubbish Bin, never a permanent delete: there is no undo on this drive.
    protected override void RemoveItem(string path, bool recurse)
    {
        if (!ShouldProcess(path, "Move to Rubbish Bin")) return;
        Backend.MoveToRubbish(ToMegaPath(path));
    }

    private static ErrorRecord NotFound(string path) =>
        new(new MegaItemNotFoundException($"'{path}' not found."), "ItemNotFound", ErrorCategory.ObjectNotFound, path);
}
