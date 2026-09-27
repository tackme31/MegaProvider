using System.Management.Automation;
using System.Management.Automation.Host;
using System.Text.RegularExpressions;
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

/// <summary>
/// Logs in and remembers the session; the mega: drive then works in this and later sessions.
/// For an account with two-factor authentication, the code is asked for when needed, or given with -AuthCode.
/// </summary>
[Cmdlet(VerbsCommunications.Connect, "MegaAccount")]
[OutputType(typeof(MegaAccountInfo))]
public sealed class ConnectMegaAccountCommand : MegaAccountCommandBase
{
    [Parameter(Mandatory = true, Position = 0)]
    [Credential]
    public PSCredential Credential { get; set; } = null!;

    /// <summary>The 6-digit code from the authenticator app. Codes change every 30 seconds.</summary>
    [Parameter]
    [ValidatePattern(@"^\d{6}$")]
    public string? AuthCode { get; set; }

    protected override void ProcessRecord() => Invoke(() =>
    {
        var password = Credential.GetNetworkCredential().Password;
        string email;
        try
        {
            email = Connect(password, AuthCode);
        }
        catch (MegaAuthCodeRequiredException) when (AuthCode is null)
        {
            // Logs in again from the start: the API takes the code together with the password.
            var code = AskForAuthCode();
            if (code is null) throw;
            email = Connect(password, code);
        }
        WriteObject(new MegaAccountInfo(email));
    }, "ConnectFailed");

    private string Connect(string password, string? authCode)
    {
        var bar = new LoginProgressBar(WriteProgress);
        try
        {
            return BackendHost.Auth.Connect(Credential.UserName, password, authCode, bar.Report);
        }
        finally
        {
            bar.Complete();
        }
    }

    /// <summary>Null where nobody can answer (pwsh -NonInteractive, a runspace without a UI).</summary>
    private string? AskForAuthCode()
    {
        string? code;
        try
        {
            Host.UI.Write("Authentication code (6 digits): ");
            code = Host.UI.ReadLine()?.Trim();
        }
        catch (Exception e) when (e is PSInvalidOperationException or NotImplementedException or HostException)
        {
            return null;
        }
        if (code is null) return null;
        return Regex.IsMatch(code, @"^\d{6}$") ? code : throw new ArgumentException("The authentication code must be 6 digits.");
    }
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
