using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public sealed class CompanionRateLimiter
{
    private readonly int capacity;
    private readonly int refillTokens;
    private readonly TimeSpan refillInterval;
    private readonly int maximumPeers;
    private readonly Dictionary<long, Dictionary<string, Bucket>> bucketsByPeer = new();
    private readonly object syncRoot = new();

    public CompanionRateLimiter(
        int capacity,
        int refillTokens,
        TimeSpan refillInterval,
        int maximumPeers = 256)
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
        if (maximumPeers <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumPeers), "Maximum peers must be positive.");
        }

        this.capacity = capacity;
        this.refillTokens = refillTokens;
        this.refillInterval = refillInterval;
        this.maximumPeers = maximumPeers;
    }

    public bool TryConsume(long peerId, string messageType, DateTimeOffset now)
    {
        if (!IsKnownMessageType(messageType))
        {
            throw new ArgumentException(
                "Message type must be an exact companion protocol message type.",
                nameof(messageType));
        }

        lock (syncRoot)
        {
            if (!bucketsByPeer.TryGetValue(peerId, out var peerBuckets))
            {
                if (bucketsByPeer.Count >= maximumPeers)
                {
                    return false;
                }

                peerBuckets = new Dictionary<string, Bucket>(StringComparer.Ordinal);
                bucketsByPeer.Add(peerId, peerBuckets);
            }

            if (!peerBuckets.TryGetValue(messageType, out var bucket))
            {
                bucket = new Bucket(capacity, now);
                peerBuckets.Add(messageType, bucket);
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

    private static bool IsKnownMessageType(string? messageType) =>
        messageType == CompanionMessageTypes.Hello
        || messageType == CompanionMessageTypes.HelloAck
        || messageType == CompanionMessageTypes.HelloNack
        || messageType == CompanionMessageTypes.Heartbeat
        || messageType == CompanionMessageTypes.Chat
        || messageType == CompanionMessageTypes.InventorySnapshot
        || messageType == CompanionMessageTypes.PlayerDeath
        || messageType == CompanionMessageTypes.EntityKilled;

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
