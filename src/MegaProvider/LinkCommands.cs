using System.Management.Automation;
using System.Net;
using System.Security;
using MegaProvider.Backend;

namespace MegaProvider;

/// <summary>What Publish-MegaItem and Get-MegaLink write: one item's public link.</summary>
public sealed class MegaLinkInfo
{
    internal MegaLinkInfo(MegaLink link, string? protectedUrl = null)
    {
        Path = MegaCloudProvider.ToDrivePath(link.Path);
        Name = link.Name;
        Handle = link.Handle;
        IsFolder = link.IsFolder;
        UnprotectedUrl = link.Url;
        Url = protectedUrl ?? link.Url;
        Created = link.Created;
        ExpiresAt = link.ExpiresAt;
        IsExpired = link.IsExpired;
        IsTakenDown = link.IsTakenDown;
        var hash = link.Url.IndexOf('#');
        if (hash >= 0)
            (UrlWithoutKey, Key) = (link.Url[..hash], link.Url[(hash + 1)..]);
    }

    /// <summary>The item on the mega: drive. Also PSPath, so the object pipes into Unpublish-MegaItem.</summary>
    public string Path { get; }
    public string Name { get; }
    public string Handle { get; }
    public bool IsFolder { get; }

    /// <summary>The link to hand out: the password-protected one when -Password was given.</summary>
    public string Url { get; }

    /// <summary>The plain link with its key. Still valid after a password-protected one is made from it.</summary>
    public string UnprotectedUrl { get; }

    public bool IsPasswordProtected => Url != UnprotectedUrl;

    /// <summary>The link and its key, to send separately (MEGA's way to protect a link on the free plan).</summary>
    public string? UrlWithoutKey { get; }
    public string? Key { get; }

    public DateTime? Created { get; }

    /// <summary>Null when the link never expires.</summary>
    public DateTime? ExpiresAt { get; }

    public bool IsExpired { get; }
    public bool IsTakenDown { get; }

    internal PSObject ToPSObject()
    {
        var pso = PSObject.AsPSObject(this);
        pso.Properties.Add(new PSNoteProperty("PSPath", Path));
        return pso;
    }
}

/// <summary>Path / -LiteralPath inputs on the mega: drive, as the transfer cmdlets take them.</summary>
public abstract class MegaPathCommandBase : MegaItemCommandBase
{
    [Parameter(Mandatory = true, Position = 0, ParameterSetName = "Path", ValueFromPipeline = true)]
    public string[] Path { get; set; } = [];

    [Parameter(Mandatory = true, ParameterSetName = "LiteralPath", ValueFromPipelineByPropertyName = true)]
    [Alias("PSPath", "LP")]
    public string[] LiteralPath { get; set; } = [];

    protected override void ProcessRecord()
    {
        var literal = ParameterSetName == "LiteralPath";
        foreach (var input in literal ? LiteralPath : Path)
            Try(input, () =>
            {
                foreach (var megaPath in ResolveMega(input, literal))
                    Process(megaPath);
            });
    }

    protected abstract void Process(string megaPath);
}

/// <summary>Creates public links, or returns the ones the items already have.</summary>
[Cmdlet(VerbsData.Publish, "MegaItem", SupportsShouldProcess = true, DefaultParameterSetName = "Path")]
[OutputType(typeof(MegaLinkInfo))]
public sealed class PublishMegaItemCommand : MegaPathCommandBase
{
    /// <summary>When the link stops working. Without this or -NoExpiry, an existing expiry is kept.</summary>
    [Parameter]
    public DateTime? ExpiresAt { get; set; }

    /// <summary>Removes the link's expiry.</summary>
    [Parameter]
    public SwitchParameter NoExpiry { get; set; }

    /// <summary>
    /// Also makes a password-protected link, returned as Url. MEGA keeps nothing of it: save it now,
    /// Get-MegaLink cannot show it later. The plain link (UnprotectedUrl) keeps working.
    /// </summary>
    [Parameter]
    public SecureString? Password { get; set; }

    private bool _ready;

    protected override void BeginProcessing() => Try("", () =>
    {
        if (ExpiresAt is not null && NoExpiry)
            throw new ArgumentException("Give either -ExpiresAt or -NoExpiry, not both.");
        if (ExpiresAt <= DateTime.Now)
            throw new ArgumentException($"-ExpiresAt {ExpiresAt} is not in the future.");
        if (Password is { Length: 0 })
            throw new ArgumentException("-Password is empty.");
        // Checked before any link is made, so a refused option leaves nothing half done. MEGA sells both
        // as Pro features; the server enforces the expiry, but a password is computed locally.
        if (ExpiresAt is not null || Password is not null)
        {
            if (!Backend.HasPaidPlan())
                throw new MegaProPlanRequiredException(ExpiresAt is not null ? "Link expiry dates" : "Password-protected links");
        }
        _ready = true;
    });

    protected override void Process(string megaPath)
    {
        if (!_ready) return;
        MegaCloudProvider.EnsureUnambiguous(megaPath);
        var target = MegaCloudProvider.ToDrivePath(megaPath);
        var action = ExpiresAt is not null ? $"Create public link expiring {ExpiresAt:g}"
            : NoExpiry ? "Create public link with no expiry"
            : "Create public link";
        if (!ShouldProcess(target, action)) return;
        // A folder takes MEGA a while (over ten seconds in MegaExplorer).
        var progress = new ProgressRecord(1, "Creating public link", target);
        WriteProgress(progress);
        var link = ExpiresAt is not null || NoExpiry ? Backend.Publish(megaPath, ExpiresAt) : Backend.Publish(megaPath);
        progress.RecordType = ProgressRecordType.Completed;
        WriteProgress(progress);
        var protectedUrl = Password is null ? null : Backend.ProtectLink(link.Url, new NetworkCredential("", Password).Password);
        WriteObject(new MegaLinkInfo(link, protectedUrl).ToPSObject());
    }
}

/// <summary>Removes public links.</summary>
[Cmdlet(VerbsData.Unpublish, "MegaItem", SupportsShouldProcess = true, DefaultParameterSetName = "Path")]
public sealed class UnpublishMegaItemCommand : MegaPathCommandBase
{
    protected override void Process(string megaPath)
    {
        MegaCloudProvider.EnsureUnambiguous(megaPath);
        if (!ShouldProcess(MegaCloudProvider.ToDrivePath(megaPath), "Remove public link")) return;
        Backend.Unpublish(megaPath);
    }
}

/// <summary>Lists public links: every one in the Cloud Drive, or those of the given items (and below, with -Recurse).</summary>
[Cmdlet(VerbsCommon.Get, "MegaLink", DefaultParameterSetName = "Path")]
[OutputType(typeof(MegaLinkInfo))]
public sealed class GetMegaLinkCommand : MegaItemCommandBase
{
    [Parameter(Position = 0, ParameterSetName = "Path", ValueFromPipeline = true)]
    public string[]? Path { get; set; }

    [Parameter(Mandatory = true, ParameterSetName = "LiteralPath", ValueFromPipelineByPropertyName = true)]
    [Alias("PSPath", "LP")]
    public string[] LiteralPath { get; set; } = [];

    [Parameter]
    public SwitchParameter Recurse { get; set; }

    protected override void ProcessRecord()
    {
        var literal = ParameterSetName == "LiteralPath";
        var inputs = literal ? LiteralPath : Path;
        if (inputs is null)
        {
            Try("", () => WriteLinks(Backend.ListLinks()));
            return;
        }
        foreach (var input in inputs)
            Try(input, () =>
            {
                foreach (var megaPath in ResolveMega(input, literal))
                {
                    if (!Recurse)
                    {
                        if (Backend.GetLink(megaPath) is { } link) WriteLinks([link]);
                        continue;
                    }
                    var item = Backend.Get(megaPath) ?? throw new MegaItemNotFoundException($"'{input}' not found.");
                    // Ignoring case like the path lookup does (cd DOCS finds docs).
                    WriteLinks(Backend.ListLinks().Where(l =>
                        l.Handle == item.Handle
                        || megaPath.Length == 0
                        || l.Path.StartsWith(megaPath + "/", StringComparison.OrdinalIgnoreCase)));
                }
            });
    }

    private void WriteLinks(IEnumerable<MegaLink> links)
    {
        foreach (var link in links)
            WriteObject(new MegaLinkInfo(link).ToPSObject());
    }
}
