namespace MegaProvider.Backend;

/// <summary>
/// Repeats a call that MEGA refused with EAGAIN (-3): the request was not applied, so it is safe to send again.
/// The SDK already backs off by itself when a whole batch is refused (megaclient.cpp, btcs); what reaches
/// us is a single command refused inside an accepted batch, which it hands back instead.
/// </summary>
public static class RateLimit
{
    public static readonly TimeSpan[] DefaultDelays =
        [TimeSpan.FromSeconds(0.5), TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(4)];

    /// <summary>Waits <paramref name="delays"/>[i] before the (i+2)th attempt; after the last, throws <see cref="MegaRateLimitedException"/>.</summary>
    public static T Retry<T>(Func<T> call, Func<Exception, bool> isAgain, IReadOnlyList<TimeSpan> delays)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return call();
            }
            catch (Exception e) when (isAgain(e))
            {
                if (attempt == delays.Count)
                    throw new MegaRateLimitedException(attempt, e);
                Thread.Sleep(delays[attempt]);
            }
        }
    }
}

public sealed class MegaRateLimitedException : Exception
{
    public MegaRateLimitedException(int retries, Exception inner)
        : base($"MEGA is limiting the request rate and refused this request {retries + 1} times (EAGAIN). " +
               "Wait a few minutes before going on.", inner) { }

    private MegaRateLimitedException(string message, Exception? inner) : base(message, inner) { }

    // In the message itself: PowerShell drops ErrorDetails from a provider's terminating error.
    /// <summary>The same error for a pipeline stopped at <paramref name="target"/>: what was and was not done.</summary>
    public MegaRateLimitedException StoppedAt(string target) =>
        new($"{Message} Stopped at '{target}': the items before it were done; it and the rest were not.", InnerException);
}
