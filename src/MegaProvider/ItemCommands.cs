using System.Collections.ObjectModel;
using System.Management.Automation;
using Microsoft.PowerShell.Commands;
using MegaProvider.Backend;

namespace MegaProvider;

/// <summary>Path plumbing shared by the cmdlets that take both mega: and local paths.</summary>
public abstract class MegaItemCommandBase : PSCmdlet
{
    protected IMegaBackend Backend => LoginProgressBar.EnsureSession(WriteProgress);

    /// <summary>Resolves a PowerShell path (wildcards expanded unless literal) that must be on a given provider.</summary>
    protected Collection<string> Resolve(string path, bool literal, Type providerType, string what)
    {
        Collection<string> paths;
        ProviderInfo provider;
        if (literal)
            paths = new() { SessionState.Path.GetUnresolvedProviderPathFromPSPath(path, out provider, out _) };
        else
            paths = SessionState.Path.GetResolvedProviderPathFromPSPath(path, out provider);
        if (provider.ImplementingType != providerType)
            throw new ArgumentException($"'{path}' is not {what}.");
        return paths;
    }

    protected Collection<string> ResolveMega(string path, bool literal) =>
        new(Resolve(path, literal, typeof(MegaCloudProvider), "on the mega: drive").Select(MegaCloudProvider.ToMegaPath).ToList());

    protected Collection<string> ResolveLocal(string path, bool literal) =>
        Resolve(path, literal, typeof(FileSystemProvider), "a file system path");

    /// <summary>Emits the item the way Get-Item would, so it carries PSPath and pipes into Rename-Item etc.</summary>
    protected void WriteMegaItem(string megaPath) =>
        WriteObject(InvokeProvider.Item.Get(new[] { MegaCloudProvider.ToDrivePath(megaPath) }, false, true), true);

    protected void WriteLocalItem(string localPath) =>
        WriteObject(InvokeProvider.Item.Get(new[] { localPath }, false, true), true);

    /// <summary>A progress callback for one transfer, shown as a PowerShell progress bar.</summary>
    protected Action<long, long> ProgressFor(string activity, string item)
    {
        var record = new ProgressRecord(1, activity, item);
        return (done, total) =>
        {
            record.PercentComplete = total > 0 ? (int)Math.Min(100, done * 100 / total) : -1;
            record.StatusDescription = $"{item}  ({Bytes.Format(done)} / {Bytes.Format(total)})";
            WriteProgress(record);
        };
    }

    // Ctrl+C: PowerShell calls this on another thread while ProcessRecord is blocked in a transfer.
    protected override void StopProcessing() => BackendHost.Backend.CancelTransfer();

    /// <summary>One failed item should not stop the rest of a pipeline, unless the next would fail the same way.</summary>
    protected void Try(string target, Action action)
    {
        try
        {
            action();
        }
        catch (MegaNotConnectedException e)
        {
            ThrowTerminatingError(new ErrorRecord(e, "NotConnected", ErrorCategory.AuthenticationError, null));
        }
        catch (MegaRateLimitedException e)
        {
            ThrowTerminatingError(new ErrorRecord(e.StoppedAt(target), "RateLimited", ErrorCategory.LimitsExceeded, target));
        }
        catch (OperationCanceledException) when (Stopping)
        {
            throw new PipelineStoppedException();
        }
        catch (Exception e) when (e is not PipelineStoppedException)
        {
            var category = e switch
            {
                MegaItemNotFoundException => ErrorCategory.ObjectNotFound,
                MegaAmbiguousPathException => ErrorCategory.InvalidArgument,
                _ => ErrorCategory.NotSpecified,
            };
            WriteError(new ErrorRecord(e, e.GetType().Name, category, target));
        }
    }
}

/// <summary>Uploads local files or folders into a mega: folder.</summary>
[Cmdlet(VerbsCommunications.Send, "MegaItem", SupportsShouldProcess = true, DefaultParameterSetName = "Path")]
[OutputType(typeof(MegaItem))]
public sealed class SendMegaItemCommand : MegaItemCommandBase
{
    [Parameter(Mandatory = true, Position = 0, ParameterSetName = "Path", ValueFromPipeline = true)]
    public string[] Path { get; set; } = [];

    [Parameter(Mandatory = true, ParameterSetName = "LiteralPath", ValueFromPipelineByPropertyName = true)]
    [Alias("PSPath", "LP")]
    public string[] LiteralPath { get; set; } = [];

    /// <summary>An existing folder on the mega: drive.</summary>
    [Parameter(Mandatory = true, Position = 1)]
    public string Destination { get; set; } = "";

    private string? _megaDestination;

    protected override void BeginProcessing() => Try(Destination, () =>
    {
        var dest = ResolveMega(Destination, literal: false);
        if (dest.Count != 1)
            throw new ArgumentException($"-Destination '{Destination}' must resolve to exactly one folder.");
        MegaCloudProvider.EnsureUnambiguous(dest[0]);
        _megaDestination = dest[0];
    });

    protected override void ProcessRecord()
    {
        if (_megaDestination is null) return;
        var literal = ParameterSetName == "LiteralPath";
        foreach (var input in literal ? LiteralPath : Path)
            Try(input, () =>
            {
                foreach (var local in ResolveLocal(input, literal))
                {
                    if (!ShouldProcess(local, $"Upload to '{Destination}'")) continue;
                    var uploaded = Backend.Upload(local, _megaDestination, ProgressFor("Uploading to MEGA", local));
                    WriteMegaItem((_megaDestination.Length > 0 ? _megaDestination + "/" : "") + uploaded.Name);
                }
            });
    }
}

/// <summary>Downloads mega: files or folders into a local folder.</summary>
[Cmdlet(VerbsCommunications.Receive, "MegaItem", SupportsShouldProcess = true, DefaultParameterSetName = "Path")]
[OutputType(typeof(FileInfo), typeof(DirectoryInfo))]
public sealed class ReceiveMegaItemCommand : MegaItemCommandBase
{
    [Parameter(Mandatory = true, Position = 0, ParameterSetName = "Path", ValueFromPipeline = true)]
    public string[] Path { get; set; } = [];

    [Parameter(Mandatory = true, ParameterSetName = "LiteralPath", ValueFromPipelineByPropertyName = true)]
    [Alias("PSPath", "LP")]
    public string[] LiteralPath { get; set; } = [];

    /// <summary>An existing local folder. Defaults to the current file system location.</summary>
    [Parameter(Position = 1)]
    public string? Destination { get; set; }

    private string? _localDestination;

    protected override void BeginProcessing() => Try(Destination ?? "", () =>
    {
        var dest = Destination is null
            ? new Collection<string> { SessionState.Path.CurrentFileSystemLocation.ProviderPath }
            : ResolveLocal(Destination, literal: false);
        if (dest.Count != 1 || !Directory.Exists(dest[0]))
            throw new ArgumentException($"-Destination '{Destination}' must be one existing local folder.");
        _localDestination = dest[0];
    });

    protected override void ProcessRecord()
    {
        if (_localDestination is null) return;
        var literal = ParameterSetName == "LiteralPath";
        foreach (var input in literal ? LiteralPath : Path)
            Try(input, () =>
            {
                foreach (var megaPath in ResolveMega(input, literal))
                {
                    MegaCloudProvider.EnsureUnambiguous(megaPath);
                    if (!ShouldProcess(MegaCloudProvider.ToDrivePath(megaPath),$"Download to '{_localDestination}'")) continue;
                    WriteLocalItem(Backend.Download(megaPath, _localDestination, ProgressFor("Downloading from MEGA", megaPath)));
                }
            });
    }
}

/// <summary>Lists the top level of the Rubbish Bin, where Remove-Item puts things.</summary>
[Cmdlet(VerbsCommon.Get, "MegaRubbishItem")]
[OutputType(typeof(MegaItem))]
public sealed class GetMegaRubbishItemCommand : MegaItemCommandBase
{
    /// <summary>Wildcard on the item name.</summary>
    [Parameter(Position = 0)]
    [SupportsWildcards]
    public string? Name { get; set; }

    protected override void ProcessRecord() => Try("Rubbish Bin", () =>
    {
        var pattern = Name is null ? null : new WildcardPattern(Name, WildcardOptions.IgnoreCase);
        foreach (var item in Backend.ListRubbish())
        {
            if (pattern is not null && !pattern.IsMatch(item.Name)) continue;
            var pso = PSObject.AsPSObject(item);
            pso.TypeNames.Insert(0, "MegaProvider.MegaRubbishItem"); // selects the view that shows Handle
            WriteObject(pso);
        }
    });
}

/// <summary>Moves Rubbish Bin items back to the folder they were removed from (or to -Destination).</summary>
[Cmdlet(VerbsData.Restore, "MegaItem", SupportsShouldProcess = true)]
[OutputType(typeof(MegaItem))]
public sealed class RestoreMegaItemCommand : MegaItemCommandBase
{
    [Parameter(Mandatory = true, Position = 0, ValueFromPipelineByPropertyName = true)]
    public string Handle { get; set; } = "";

    /// <summary>Only used in ShouldProcess messages; binds from Get-MegaRubbishItem output.</summary>
    [Parameter(ValueFromPipelineByPropertyName = true, DontShow = true)]
    public string? Name { get; set; }

    /// <summary>A mega: folder to restore into instead of the original one.</summary>
    [Parameter]
    public string? Destination { get; set; }

    protected override void ProcessRecord() => Try(Handle, () =>
    {
        string? dest = null;
        if (Destination is not null)
        {
            var resolved = ResolveMega(Destination, literal: false);
            if (resolved.Count != 1)
                throw new ArgumentException($"-Destination '{Destination}' must resolve to exactly one folder.");
            MegaCloudProvider.EnsureUnambiguous(resolved[0]);
            dest = resolved[0];
        }
        var target = Name is null ? $"H:{Handle}" : $"'{Name}' (H:{Handle})";
        if (!ShouldProcess(target, Destination is null ? "Restore to original folder" : $"Restore to '{Destination}'")) return;
        WriteMegaItem(Backend.Restore(Handle, dest));
    });
}
