namespace Takaro.Valheim.Companion;

/// <summary>Token bucket on a monotonic clock, so a burst of kills cannot flood the server.</summary>
public sealed class CompanionSendLimiter
{
    private readonly int capacity;
    private readonly double refillPerSecond;
    private double tokens;
    private TimeSpan? lastRefill;

    public CompanionSendLimiter(int capacity, double refillPerSecond)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity));
        }
        if (refillPerSecond <= 0 || double.IsNaN(refillPerSecond) || double.IsInfinity(refillPerSecond))
        {
            throw new ArgumentOutOfRangeException(nameof(refillPerSecond));
        }

        this.capacity = capacity;
        this.refillPerSecond = refillPerSecond;
        tokens = capacity;
    }

    public bool TryConsume(TimeSpan monotonicNow)
    {
        if (lastRefill is TimeSpan last && monotonicNow > last)
        {
            tokens = Math.Min(capacity, tokens + ((monotonicNow - last).TotalSeconds * refillPerSecond));
        }

        if (lastRefill is null || monotonicNow > lastRefill.Value)
        {
            lastRefill = monotonicNow;
        }

        if (tokens < 1)
        {
            return false;
        }

        tokens -= 1;
        return true;
    }
}
