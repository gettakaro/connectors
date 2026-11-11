namespace Takaro.Valheim.Core;

/// <summary>
/// When to dial Takaro again after a connection ends. A drop of a connection that Takaro had
/// accepted and that stayed up is almost always a network cut, so the first retry comes within a
/// second. Only consecutive failures back off: exponentially, with jitter, capped at 60 s.
/// </summary>
public static class ReconnectPolicy
{
    // TLS and the identify round trip add about a second, so drop to identified stays under 2 s.
    public static readonly TimeSpan FastRetryMinimum = TimeSpan.FromMilliseconds(300);
    public static readonly TimeSpan FastRetryJitter = TimeSpan.FromMilliseconds(500);
    public static readonly TimeSpan Maximum = TimeSpan.FromSeconds(60);
    public static readonly TimeSpan DefaultBackoffUnit = TimeSpan.FromSeconds(2);

    /// <summary>A connection must have been identified this long before its drop resets the streak.</summary>
    public static readonly TimeSpan StableAfter = TimeSpan.FromSeconds(10);

    /// <summary>
    /// The delay before the next attempt.
    /// </summary>
    /// <param name="failureStreak">
    /// Attempts that failed since the last stable connection; 0 means the connection that just
    /// ended was identified and stable.
    /// </param>
    /// <param name="backoffUnit">The first backoff step; doubled per further failure.</param>
    /// <param name="random">A value in [0, 1).</param>
    public static TimeSpan NextDelay(int failureStreak, TimeSpan backoffUnit, double random)
    {
        random = Math.Min(Math.Max(random, 0), 1);
        if (failureStreak <= 0)
        {
            return FastRetryMinimum + TimeSpan.FromMilliseconds(FastRetryJitter.TotalMilliseconds * random);
        }

        var unit = Math.Min(Math.Max(backoffUnit.TotalMilliseconds, 100), Maximum.TotalMilliseconds);
        var ceiling = Math.Min(unit * Math.Pow(2, Math.Min(failureStreak - 1, 16)), Maximum.TotalMilliseconds);

        // Equal jitter: at least half the ceiling, so the wait still grows with the streak.
        return TimeSpan.FromMilliseconds(ceiling / 2 + ceiling / 2 * random);
    }

    /// <summary>
    /// Whether an ended connection resets the failure streak: Takaro accepted its identify and it
    /// stayed up for <see cref="StableAfter"/>. A peer that accepts and then drops every socket
    /// keeps backing off.
    /// </summary>
    public static bool WasStable(DateTimeOffset? identifiedAt, DateTimeOffset endedAt) =>
        identifiedAt is { } at && endedAt - at >= StableAfter;
}
