using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class InventorySnapshotCacheTests
{
    [TestMethod]
    public void StoresInventorySnapshotByPlayerIdentifiers()
    {
        var cache = new InventorySnapshotCache();
        var player = new TakaroPlayer(
            GameId: "Steam_76561198000735875",
            Name: "Odin",
            SteamId: "76561198000735875",
            PlatformId: "steam:76561198000735875",
            Ip: null,
            Ping: null);
        var items = new[]
        {
            new TakaroInventoryItem("Wood", "Wood", 2, "1", 100, false, new TakaroInventorySlot(0, 1))
        };

        cache.Store(player, items, DateTimeOffset.UtcNow);

        Assert.IsTrue(cache.TryGet("Steam_76561198000735875", out var byGameId));
        Assert.AreEqual("Wood", byGameId[0].Code);
        Assert.IsTrue(cache.TryGet("76561198000735875", out var bySteamId));
        Assert.AreEqual(2, bySteamId[0].Amount);
        Assert.IsTrue(cache.TryGet("steam:76561198000735875", out _));
        Assert.IsTrue(cache.TryGet("Odin", out _));
    }

    [TestMethod]
    public void ReplacesExistingSnapshotForAliases()
    {
        var cache = new InventorySnapshotCache();
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);

        cache.Store(player, new[] { new TakaroInventoryItem("Wood", "Wood", 1, "1") }, DateTimeOffset.UtcNow);
        cache.Store(player, new[] { new TakaroInventoryItem("Stone", "Stone", 3, "1") }, DateTimeOffset.UtcNow);

        Assert.IsTrue(cache.TryGet("Steam_1", out var items));
        Assert.AreEqual(1, items.Count);
        Assert.AreEqual("Stone", items[0].Code);
    }

    [TestMethod]
    public void DoesNotReplaceNonEmptySnapshotWithTransientEmptySnapshot()
    {
        var cache = new InventorySnapshotCache();
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);

        cache.Store(player, new[] { new TakaroInventoryItem("Wood", "Wood", 1, "1") }, DateTimeOffset.UtcNow);
        cache.Store(player, Array.Empty<TakaroInventoryItem>(), DateTimeOffset.UtcNow.AddSeconds(10));

        Assert.IsTrue(cache.TryGet("Steam_1", out var items));
        Assert.AreEqual(1, items.Count);
        Assert.AreEqual("Wood", items[0].Code);
    }

    [TestMethod]
    public void DoesNotReplaceNewerSnapshotWithOlderSnapshot()
    {
        var cache = new InventorySnapshotCache();
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);
        var now = DateTimeOffset.UtcNow;

        cache.Store(player, new[] { new TakaroInventoryItem("Wood", "Wood", 3, "1") }, now);
        cache.Store(player, new[] { new TakaroInventoryItem("Wood", "Wood", 2, "1") }, now.AddSeconds(-1));

        Assert.IsTrue(cache.TryGet("Steam_1", out var items));
        Assert.AreEqual(1, items.Count);
        Assert.AreEqual(3, items[0].Amount);
    }
}
