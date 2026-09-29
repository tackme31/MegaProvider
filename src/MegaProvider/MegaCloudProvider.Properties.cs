using System.Collections;
using System.Collections.ObjectModel;
using System.Management.Automation;
using MegaProvider.Backend;

namespace MegaProvider;

// Get-, Set- and Clear-ItemProperty. Only IsFavorite and Label can be changed; MEGA's attributes are a
// fixed set, so New-/Remove-/Rename-ItemProperty (IDynamicPropertyCmdletProvider) are not offered.
public sealed partial class MegaCloudProvider
{
    private static readonly string[] DefaultProperties =
        ["Name", "Length", "LastWriteTime", "CreationTime", "IsFavorite", "HasLink", "Label", "Handle"];

    public void GetProperty(string path, Collection<string>? providerSpecificPickList)
    {
        var item = Backend.Get(ToMegaPath(path));
        if (item is null)
        {
            WriteError(NotFound(path));
            return;
        }
        // Through PSObject, so the file system's names from MegaProvider.types.ps1xml work too.
        var source = PSObject.AsPSObject(item);
        var result = new PSObject();
        var picks = providerSpecificPickList is { Count: > 0 } ? providerSpecificPickList : (IEnumerable<string>)DefaultProperties;
        foreach (var pick in picks)
        {
            var pattern = new WildcardPattern(pick, WildcardOptions.IgnoreCase);
            var matches = source.Properties.Where(p => pattern.IsMatch(p.Name)).ToList();
            if (matches.Count == 0 && !WildcardPattern.ContainsWildcardCharacters(pick))
                WriteError(new ErrorRecord(new PSArgumentException($"'{path}' has no property named '{pick}'."),
                    "PropertyNotFound", ErrorCategory.ObjectNotFound, pick));
            foreach (var p in matches)
                if (result.Properties[p.Name] is null)
                    result.Properties.Add(new PSNoteProperty(p.Name, p.Value));
        }
        if (result.Properties.Any())
            WritePropertyObject(result, path);
    }

    public object? GetPropertyDynamicParameters(string path, Collection<string>? providerSpecificPickList) => null;

    public void SetProperty(string path, PSObject propertyValue) => StopIfRateLimited(path, () =>
    {
        var changes = new List<(string Name, object? Value)>();
        foreach (var (name, value) in PropertiesOf(propertyValue))
            if (Settable(name) is { } canonical)
            {
                try
                {
                    changes.Add(canonical == "IsFavorite" ? (canonical, ToFavorite(value)) : (canonical, ToLabel(value)));
                }
                catch (PSArgumentException e)
                {
                    WriteError(new ErrorRecord(e, "InvalidPropertyValue", ErrorCategory.InvalidArgument, value));
                }
            }
        // Nothing changes unless every property given can be set: a half-applied -InputObject is harder to reason about.
        if (changes.Count > 0 && changes.Count == PropertiesOf(propertyValue).Count())
            Apply(path, changes);
    });

    public object? SetPropertyDynamicParameters(string path, PSObject propertyValue) => null;

    public void ClearProperty(string path, Collection<string> propertyToClear) => StopIfRateLimited(path, () =>
    {
        var changes = propertyToClear.Select(Settable).ToList();
        if (changes.Count > 0 && changes.All(c => c is not null))
            Apply(path, changes.Select(c => (c!, c == "IsFavorite" ? (object?)false : null)).ToList());
    });

    public object? ClearPropertyDynamicParameters(string path, Collection<string> propertyToClear) => null;

    // Set-ItemProperty -Name/-Value arrives as one note property; -InputObject as the object itself,
    // where a hashtable's own properties (Keys, Count) would be meaningless, so its entries are used.
    private static IEnumerable<(string Name, object? Value)> PropertiesOf(PSObject o) =>
        o.BaseObject is IDictionary d
            ? d.Keys.Cast<object>().Select(k => (k.ToString()!, d[k]))
            : o.Properties.Select(p => (p.Name, (object?)p.Value));

    /// <summary>The property's own spelling if it can be set, else null after writing why not.</summary>
    private string? Settable(string name)
    {
        foreach (var canonical in new[] { "IsFavorite", "Label" })
            if (string.Equals(name, canonical, StringComparison.OrdinalIgnoreCase))
                return canonical;
        var reason = name.ToLowerInvariant() switch
        {
            "name" => "'Name' is changed with Rename-Item.",
            "haslink" => "'HasLink' is changed with Publish-MegaItem and Unpublish-MegaItem.",
            "length" or "lastwritetime" or "creationtime" or "handle" or "mode" or "isfolder" or "size" or "modified" =>
                $"'{name}' is read-only.",
            _ => $"There is no property named '{name}'.",
        };
        WriteError(new ErrorRecord(new PSArgumentException(reason + " Only IsFavorite and Label can be set."),
            "PropertyNotSettable", ErrorCategory.InvalidArgument, name));
        return null;
    }

    private static object? Unwrap(object? value) => value is PSObject o ? o.BaseObject : value;

    // Not LanguagePrimitives: it turns any non-empty string, "false" included, into $true.
    private static bool ToFavorite(object? value) => Unwrap(value) switch
    {
        bool b => b,
        SwitchParameter s => s.IsPresent,
        string s when bool.TryParse(s.Trim(), out var b) => b,
        var v => throw new PSArgumentException($"IsFavorite must be $true or $false, not '{v}'."),
    };

    private static MegaLabel? ToLabel(object? value)
    {
        var v = Unwrap(value);
        MegaLabel? label = v switch
        {
            null => null,
            MegaLabel l => l,
            // By name only: Enum.TryParse would also take "5" and "Red, Orange" (which it ORs into Yellow).
            string s => Enum.GetValues<MegaLabel>().FirstOrDefault(l => string.Equals(l.ToString(), s.Trim(), StringComparison.OrdinalIgnoreCase)),
            byte or short or int or long => (MegaLabel)Convert.ToInt32(v),
            _ => (MegaLabel)0,
        };
        return label is null || Enum.IsDefined(label.Value)
            ? label
            : throw new PSArgumentException(
                $"Label must be one of {string.Join(", ", Enum.GetNames<MegaLabel>())}, or $null to remove it; not '{v}'.");
    }

    private void Apply(string path, List<(string Name, object? Value)> changes)
    {
        var megaPath = ToMegaPath(path);
        if (megaPath.Length == 0)
        {
            WriteError(new ErrorRecord(new InvalidOperationException("The drive root has no favourite or label."),
                "RootNotSettable", ErrorCategory.InvalidOperation, path));
            return;
        }
        if (!IsUnambiguous(path)) return;
        var item = Backend.Get(megaPath);
        if (item is null)
        {
            WriteError(NotFound(path));
            return;
        }
        var description = string.Join(", ", changes.Select(c => $"{c.Name} = {c.Value ?? "(none)"}"));
        if (!ShouldProcess(path, $"Set {description}")) return;
        var result = new PSObject();
        foreach (var (name, value) in changes)
        {
            // Unchanged values skip the request: fewer calls in a bulk change means less chance of EAGAIN.
            if (name == "IsFavorite" && item.IsFavorite != (bool)value!)
                item = Backend.SetFavorite(megaPath, (bool)value);
            else if (name == "Label" && item.Label != (MegaLabel?)value)
                item = Backend.SetLabel(megaPath, (MegaLabel?)value);
            result.Properties.Add(new PSNoteProperty(name, value));
        }
        WritePropertyObject(result, path);
    }
}
