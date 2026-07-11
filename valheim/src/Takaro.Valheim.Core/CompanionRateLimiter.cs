namespace Takaro.Valheim.Core;

public sealed class CompanionRateLimiter
{
    private readonly int capacity;
    private readonly int refillTokens;
    private readonly TimeSpan refillInterval;
    private readonly Dictionary<long, Dictionary<string, Bucket>> bucketsByPeer = new();
    private readonly object syncRoot = new();

    public CompanionRateLimiter(int capacity, int refillTokens, TimeSpan refillInterval)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity), "Capacity must be positive.");
        }
        if (refillTokens <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(refillTokens), "Refill rate must be positive.");
        }
        if (refillInterval <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(refillInterval), "Refill interval must be positive.");
        }

        this.capacity = capacity;
        this.refillTokens = refillTokens;
        this.refillInterval = refillInterval;
    }

    public bool TryConsume(long peerId, string messageType, DateTimeOffset now)
    {
        if (string.IsNullOrWhiteSpace(messageType))
        {
            throw new ArgumentException("Message type must not be blank.", nameof(messageType));
        }

        var normalizedMessageType = messageType.Trim();
        lock (syncRoot)
        {
            if (!bucketsByPeer.TryGetValue(peerId, out var peerBuckets))
            {
                peerBuckets = new Dictionary<string, Bucket>(StringComparer.Ordinal);
                bucketsByPeer.Add(peerId, peerBuckets);
            }

            if (!peerBuckets.TryGetValue(normalizedMessageType, out var bucket))
            {
                bucket = new Bucket(capacity, now);
                peerBuckets.Add(normalizedMessageType, bucket);
            }
            else
            {
                Refill(bucket, now);
            }

            if (bucket.Tokens < 1m)
            {
                return false;
            }

            bucket.Tokens -= 1m;
            return true;
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            bucketsByPeer.Remove(peerId);
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            bucketsByPeer.Clear();
        }
    }

    private void Refill(Bucket bucket, DateTimeOffset now)
    {
        if (now <= bucket.LastRefillAt)
        {
            return;
        }

        var elapsedTicks = (now - bucket.LastRefillAt).Ticks;
        var replenished = (decimal)elapsedTicks * refillTokens / refillInterval.Ticks;
        bucket.Tokens = Math.Min(capacity, bucket.Tokens + replenished);
        bucket.LastRefillAt = now;
    }

    private sealed class Bucket
    {
        public Bucket(int initialTokens, DateTimeOffset lastRefillAt)
        {
            Tokens = initialTokens;
            LastRefillAt = lastRefillAt;
        }

        public decimal Tokens { get; set; }

        public DateTimeOffset LastRefillAt { get; set; }
    }
}
