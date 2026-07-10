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
}
