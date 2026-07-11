using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PlayerPositionCacheTests
{
    private static readonly DateTimeOffset Now = DateTimeOffset.Parse("2026-07-10T15:30:00+02:00");

    [TestMethod]
    public void RetainsFreshServerObservedPositionAcrossDisconnectAliases()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);

        cache.Remember(player, new TakaroPosition(12, 34, 56, "valheim"), Now);

        Assert.IsTrue(cache.TryGet("Steam_1", Now.AddSeconds(10), out var byGameId));
        Assert.AreEqual(12, byGameId.X);
        Assert.IsTrue(cache.TryGet("steam:1", Now.AddSeconds(10), out var byPlatformId));
        Assert.AreEqual(56, byPlatformId.Z);
        Assert.IsTrue(cache.TryGet("Odin", Now.AddSeconds(10), out _));
    }

    [TestMethod]
    public void KeepsObservationsPlayerKeyed()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        cache.Remember(new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null), new TakaroPosition(1, 2, 3, "valheim"), Now);
        cache.Remember(new TakaroPlayer("Steam_2", "Freya", "2", "steam:2", null, null), new TakaroPosition(7, 8, 9, "valheim"), Now);

        Assert.IsTrue(cache.TryGet("Steam_1", Now, out var odin));
        Assert.IsTrue(cache.TryGet("Steam_2", Now, out var freya));
        Assert.AreEqual(1, odin.X);
        Assert.AreEqual(7, freya.X);
    }

    [TestMethod]
    public void RejectsExpiredObservations()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        cache.Remember(new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null), new TakaroPosition(1, 2, 3, "valheim"), Now);

        Assert.IsFalse(cache.TryGet("Steam_1", Now.AddSeconds(31), out _));
    }

    [TestMethod]
    public void NeverStoresOriginAsAnUnavailablePlaceholder()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);

        var remembered = cache.Remember(player, new TakaroPosition(0, 0, 0, "valheim"), Now);

        Assert.IsFalse(remembered);
        Assert.IsFalse(cache.TryGet("Steam_1", Now, out _));
    }

    [TestMethod]
    public void WorldResetClearsAllObservations()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        cache.Remember(new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null), new TakaroPosition(1, 2, 3, "valheim"), Now);

        cache.Clear();

        Assert.IsFalse(cache.TryGet("Steam_1", Now, out _));
    }

    [TestMethod]
    public async Task StaleWorldRememberCannotClearOrOverwriteNewWorldObservation()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        var oldWorld = new object();
        var newWorld = new object();
        var oldPlayer = new TakaroPlayer("Steam_old", "Old", "old", "steam:old", null, null);
        var newPlayer = new TakaroPlayer("Steam_new", "New", "new", "steam:new", null, null);
        var oldCanContinue = new ManualResetEventSlim(false);
        var oldStarted = new ManualResetEventSlim(false);

        cache.SwitchWorld(oldWorld);
        var delayedOldRemember = Task.Run(() =>
        {
            oldStarted.Set();
            oldCanContinue.Wait();
            return cache.RememberIfCurrentWorld(oldWorld, oldPlayer, new TakaroPosition(1, 2, 3, "valheim"), Now);
        });

        oldStarted.Wait();
        cache.SwitchWorld(newWorld);
        Assert.IsTrue(cache.RememberIfCurrentWorld(newWorld, newPlayer, new TakaroPosition(4, 5, 6, "valheim"), Now));
        oldCanContinue.Set();

        Assert.IsFalse(await delayedOldRemember);
        Assert.IsFalse(cache.TryGetForCurrentWorld(newWorld, oldPlayer.GameId, Now, out _));
        Assert.IsTrue(cache.TryGetForCurrentWorld(newWorld, newPlayer.GameId, Now, out var current));
        Assert.AreEqual(4, current.X);
    }

    [TestMethod]
    public async Task ParallelReadsWritesExpiryAndClearRemainSafe()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromMilliseconds(25));
        var start = new ManualResetEventSlim(false);
        var workers = Enumerable.Range(0, 16).Select(worker => Task.Run(() =>
        {
            start.Wait();
            for (var iteration = 0; iteration < 10_000; iteration++)
            {
                var playerNumber = (worker + iteration) % 64;
                var player = new TakaroPlayer(
                    $"Steam_{playerNumber}",
                    $"Viking_{playerNumber}",
                    playerNumber.ToString(),
                    $"steam:{playerNumber}",
                    null,
                    null);
                var observedAt = Now.AddMilliseconds(iteration);
                cache.Remember(player, new TakaroPosition(playerNumber + 1, worker + 1, iteration + 1, "valheim"), observedAt);
                cache.TryGet(player.GameId, observedAt.AddMilliseconds(iteration % 30), out _);

                if (iteration % 257 == 0)
                {
                    cache.Clear();
                }
            }
        })).ToArray();

        start.Set();
        await Task.WhenAll(workers);

        cache.Clear();
        Assert.IsFalse(cache.TryGet("Steam_1", Now.AddDays(1), out _));
    }

    [TestMethod]
    public async Task ConcurrentExpiryPrunesOnlyExpiredObservationsAndClearRemainsAuthoritative()
    {
        var cache = new PlayerPositionCache(TimeSpan.FromSeconds(30));
        var stale = new TakaroPlayer("Steam_stale", "Stale", "stale", "steam:stale", null, null);
        var fresh = new TakaroPlayer("Steam_fresh", "Fresh", "fresh", "steam:fresh", null, null);
        cache.Remember(stale, new TakaroPosition(1, 2, 3, "valheim"), Now.AddMinutes(-1));
        cache.Remember(fresh, new TakaroPosition(4, 5, 6, "valheim"), Now);

        await Task.WhenAll(
            Task.Run(() => Parallel.For(0, 2_000, iteration => cache.TryGet(stale.GameId, Now, out _))),
            Task.Run(() => Parallel.For(0, 2_000, iteration => cache.Remember(fresh, new TakaroPosition(4, 5, 6, "valheim"), Now))),
            Task.Run(() => Parallel.For(0, 2_000, iteration => cache.TryGet(fresh.Name, Now, out _))));

        Assert.IsFalse(cache.TryGet(stale.Name, Now, out _));
        Assert.IsTrue(cache.TryGet(fresh.GameId, Now, out var position));
        Assert.AreEqual(4, position.X);

        cache.Clear();
        Assert.IsFalse(cache.TryGet(fresh.GameId, Now, out _));
    }
}
