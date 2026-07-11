using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionValidationTests
{
    private static readonly DateTimeOffset Now =
        DateTimeOffset.Parse("2026-07-11T10:00:00+02:00");

    [TestMethod]
    public void RateLimitsArePerPeerAndMessageType()
    {
        var limiter = new CompanionRateLimiter(
            capacity: 1,
            refillTokens: 1,
            refillInterval: TimeSpan.FromSeconds(10));

        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsFalse(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsTrue(limiter.TryConsume(2, CompanionMessageTypes.Chat, Now));
        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.InventorySnapshot, Now));

        limiter.RemovePeer(1);
        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        limiter.Clear();
        Assert.IsTrue(limiter.TryConsume(2, CompanionMessageTypes.Chat, Now));
    }

    [TestMethod]
    public void RateLimitRefillsFromInjectedTime()
    {
        var limiter = new CompanionRateLimiter(
            capacity: 2,
            refillTokens: 1,
            refillInterval: TimeSpan.FromSeconds(10));

        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsFalse(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsFalse(
            limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddMinutes(-1)),
            "Regressive time must neither mint tokens nor corrupt refill state.");
        Assert.IsFalse(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddSeconds(9)));
        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddSeconds(10)));
        Assert.IsFalse(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddSeconds(10)));

        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddMinutes(10)));
        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddMinutes(10)));
        Assert.IsFalse(
            limiter.TryConsume(1, CompanionMessageTypes.Chat, Now.AddMinutes(10)),
            "A long idle interval must not refill above capacity.");
    }

    [TestMethod]
    public void RateLimiterBoundsPeersAndRecoversAfterRemoval()
    {
        var limiter = new CompanionRateLimiter(
            capacity: 1,
            refillTokens: 1,
            refillInterval: TimeSpan.FromMinutes(1),
            maximumPeers: 2);

        Assert.IsTrue(limiter.TryConsume(1, CompanionMessageTypes.Chat, Now));
        Assert.IsTrue(limiter.TryConsume(2, CompanionMessageTypes.Chat, Now));
        Assert.IsFalse(limiter.TryConsume(3, CompanionMessageTypes.Chat, Now));
        Assert.IsTrue(
            limiter.TryConsume(1, CompanionMessageTypes.InventorySnapshot, Now),
            "Existing peers must be able to create another bounded message-type bucket.");

        limiter.RemovePeer(2);

        Assert.IsTrue(limiter.TryConsume(3, CompanionMessageTypes.Chat, Now));
    }

    [TestMethod]
    public void RateLimiterAcceptsOnlyExactProtocolMessageTypes()
    {
        var limiter = new CompanionRateLimiter(
            capacity: 1,
            refillTokens: 1,
            refillInterval: TimeSpan.FromMinutes(1));
        var knownTypes = new[]
        {
            CompanionMessageTypes.Hello,
            CompanionMessageTypes.HelloAck,
            CompanionMessageTypes.Heartbeat,
            CompanionMessageTypes.Chat,
            CompanionMessageTypes.InventorySnapshot,
            CompanionMessageTypes.PlayerDeath,
            CompanionMessageTypes.EntityKilled
        };

        foreach (var messageType in knownTypes)
        {
            Assert.IsTrue(limiter.TryConsume(1, messageType, Now), messageType);
        }

        Assert.ThrowsException<ArgumentException>(() =>
            limiter.TryConsume(1, "CHAT", Now));
        Assert.ThrowsException<ArgumentException>(() =>
            limiter.TryConsume(1, "unknown", Now));
        Assert.ThrowsException<ArgumentException>(() =>
            limiter.TryConsume(1, new string('x', 1_024), Now));
    }

    [TestMethod]
    public void RateLimiterDefaultPeerBoundIsFinite()
    {
        var limiter = new CompanionRateLimiter(
            capacity: 1,
            refillTokens: 1,
            refillInterval: TimeSpan.FromMinutes(1));

        for (var peerId = 1; peerId <= 256; peerId++)
        {
            Assert.IsTrue(limiter.TryConsume(peerId, CompanionMessageTypes.Chat, Now));
        }

        Assert.IsFalse(limiter.TryConsume(257, CompanionMessageTypes.Chat, Now));
    }

    [TestMethod]
    public void DuplicateEventIdIsAcceptedExactlyOnce()
    {
        var deduplicator = new BoundedEventDeduplicator(capacity: 4);

        Assert.IsTrue(deduplicator.TryAccept(1, "event-1"));
        Assert.IsFalse(deduplicator.TryAccept(1, "event-1"));
        Assert.IsTrue(deduplicator.TryAccept(2, "event-1"));

        Assert.ThrowsException<ArgumentException>(() => deduplicator.TryAccept(1, " "));
        Assert.ThrowsException<ArgumentException>(() => deduplicator.TryAccept(
            1,
            new string('e', CompanionProtocol.MaximumEventIdCharacters + 1)));
    }

    [TestMethod]
    public void DuplicateEventIdsAreScopedToBoundedSessionNonce()
    {
        var deduplicator = new BoundedEventDeduplicator(capacity: 4);

        Assert.IsTrue(deduplicator.TryAccept(1, "nonce-old", "event-1"));
        Assert.IsFalse(deduplicator.TryAccept(1, "nonce-old", "event-1"));
        Assert.IsTrue(deduplicator.TryAccept(1, "nonce-new", "event-1"));
        Assert.IsFalse(deduplicator.TryAccept(1, "nonce-new", "event-1"));

        Assert.ThrowsException<ArgumentException>(() =>
            deduplicator.TryAccept(1, " ", "event-2"));
        Assert.ThrowsException<ArgumentException>(() => deduplicator.TryAccept(
            1,
            new string('n', CompanionEnvelopeCodec.MaximumSessionNonceCharacters + 1),
            "event-2"));
    }

    [TestMethod]
    public void DeduplicatorEvictsOldestEntryAtBound()
    {
        var deduplicator = new BoundedEventDeduplicator(capacity: 2);

        Assert.IsTrue(deduplicator.TryAccept(1, "oldest"));
        Assert.IsTrue(deduplicator.TryAccept(1, "newer"));
        Assert.IsFalse(
            deduplicator.TryAccept(1, "oldest"),
            "A duplicate must not refresh its eviction order.");
        Assert.IsTrue(deduplicator.TryAccept(2, "newest"));
        Assert.IsTrue(
            deduplicator.TryAccept(1, "oldest"),
            "The deterministic oldest entry must be accepted again after eviction.");
        Assert.IsTrue(
            deduplicator.TryAccept(1, "newer"),
            "Reaccepting the oldest entry must evict the next-oldest retained entry.");
    }

    [TestMethod]
    public void DeduplicatorLifecycleRemovalKeepsIndexAndOrderConsistent()
    {
        var deduplicator = new BoundedEventDeduplicator(capacity: 3);
        Assert.IsTrue(deduplicator.TryAccept(1, "peer-one-a"));
        Assert.IsTrue(deduplicator.TryAccept(2, "peer-two"));
        Assert.IsTrue(deduplicator.TryAccept(1, "peer-one-b"));

        deduplicator.RemovePeer(1);

        Assert.IsTrue(deduplicator.TryAccept(1, "peer-one-a"));
        Assert.IsFalse(deduplicator.TryAccept(2, "peer-two"));
        deduplicator.Clear();
        Assert.IsTrue(deduplicator.TryAccept(2, "peer-two"));
    }

    [TestMethod]
    public async Task ConcurrentValidationIsThreadSafeAndBounded()
    {
        const int capacity = 64;
        var limiter = new CompanionRateLimiter(
            capacity,
            refillTokens: 1,
            refillInterval: TimeSpan.FromHours(1));
        var acceptedTokens = 0;

        await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => Task.Run(() =>
            Parallel.For(0, 1_000, _ =>
            {
                if (limiter.TryConsume(1, CompanionMessageTypes.Chat, Now))
                {
                    Interlocked.Increment(ref acceptedTokens);
                }
            }))));

        Assert.AreEqual(capacity, acceptedTokens);

        var deduplicator = new BoundedEventDeduplicator(capacity);
        var acceptedEvents = 0;
        await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => Task.Run(() =>
            Parallel.For(0, capacity, eventNumber =>
            {
                if (deduplicator.TryAccept(1, $"event-{eventNumber}"))
                {
                    Interlocked.Increment(ref acceptedEvents);
                }
            }))));

        Assert.AreEqual(capacity, acceptedEvents);
    }

    [TestMethod]
    public void InvalidValidationConfigurationIsRejected()
    {
        Assert.ThrowsException<ArgumentOutOfRangeException>(() =>
            new CompanionRateLimiter(0, 1, TimeSpan.FromSeconds(1)));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() =>
            new CompanionRateLimiter(1, 0, TimeSpan.FromSeconds(1)));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() =>
            new CompanionRateLimiter(1, 1, TimeSpan.Zero));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() =>
            new CompanionRateLimiter(1, 1, TimeSpan.FromSeconds(1), maximumPeers: 0));
        Assert.ThrowsException<ArgumentException>(() =>
            new CompanionRateLimiter(1, 1, TimeSpan.FromSeconds(1)).TryConsume(1, " ", Now));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() =>
            new BoundedEventDeduplicator(0));
    }
}
