using System.Management.Automation;
using MegaProvider.Backend;

namespace MegaProvider;

public sealed record MegaAccountInfo(string Email);

public abstract class MegaAccountCommandBase : PSCmdlet
{
    protected void Invoke(Action action, string errorId)
    {
        try
        {
            action();
        }
        catch (MegaNotConnectedException e)
        {
            ThrowTerminatingError(new ErrorRecord(e, "NotConnected", ErrorCategory.AuthenticationError, null));
        }
        catch (Exception e) when (e is not PipelineStoppedException)
        {
            ThrowTerminatingError(new ErrorRecord(e, errorId, ErrorCategory.NotSpecified, null));
        }
    }
}

/// <summary>Logs in and remembers the session; the mega: drive then works in this and later sessions.</summary>
[Cmdlet(VerbsCommunications.Connect, "MegaAccount")]
[OutputType(typeof(MegaAccountInfo))]
public sealed class ConnectMegaAccountCommand : MegaAccountCommandBase
{
    [Parameter(Mandatory = true, Position = 0)]
    [Credential]
    public PSCredential Credential { get; set; } = null!;

    protected override void ProcessRecord() => Invoke(() =>
    {
        var bar = new LoginProgressBar(WriteProgress);
        try
        {
            var email = BackendHost.Auth.Connect(Credential.UserName, Credential.GetNetworkCredential().Password, bar.Report);
            WriteObject(new MegaAccountInfo(email));
        }
        finally
        {
            bar.Complete();
        }
    }, "ConnectFailed");
}

[Cmdlet(VerbsCommon.Get, "MegaAccount")]
[OutputType(typeof(MegaAccountInfo))]
public sealed class GetMegaAccountCommand : MegaAccountCommandBase
{
    protected override void ProcessRecord() =>
        Invoke(() =>
        {
            LoginProgressBar.EnsureSession(WriteProgress);
            WriteObject(new MegaAccountInfo(BackendHost.Auth.GetAccountEmail()));
        }, "GetAccountFailed");
}

/// <summary>Logs out (invalidating the session on the server) and forgets it.</summary>
[Cmdlet(VerbsCommunications.Disconnect, "MegaAccount")]
public sealed class DisconnectMegaAccountCommand : MegaAccountCommandBase
{
    protected override void ProcessRecord() => Invoke(() => BackendHost.Auth.Disconnect(), "DisconnectFailed");
}
