using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class LocationSnapshotCacheTests
{
    [TestMethod]
    public void StoresLocationSnapshotByPlayerIdentifiers()
    {
        var cache = new LocationSnapshotCache();
        var player = new TakaroPlayer(
            GameId: "Steam_76561198000735875",
            Name: "Odin",
            SteamId: "76561198000735875",
            PlatformId: "steam:76561198000735875",
            Ip: null,
            Ping: null);

        cache.Store(player, new TakaroPosition(12.5, 34.25, -9.75, "valheim"), DateTimeOffset.UtcNow);

        Assert.IsTrue(cache.TryGet("Steam_76561198000735875", out var byGameId));
        Assert.AreEqual(12.5, byGameId.X);
        Assert.AreEqual(34.25, byGameId.Y);
        Assert.AreEqual(-9.75, byGameId.Z);
        Assert.AreEqual("valheim", byGameId.Dimension);
        Assert.IsTrue(cache.TryGet("76561198000735875", out _));
        Assert.IsTrue(cache.TryGet("steam:76561198000735875", out _));
        Assert.IsTrue(cache.TryGet("Odin", out _));
    }

    [TestMethod]
    public void DoesNotReplaceNewerLocationSnapshotWithOlderSnapshot()
    {
        var cache = new LocationSnapshotCache();
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);
        var now = DateTimeOffset.UtcNow;

        cache.Store(player, new TakaroPosition(10, 20, 30, "valheim"), now);
        cache.Store(player, new TakaroPosition(1, 2, 3, "valheim"), now.AddSeconds(-1));

        Assert.IsTrue(cache.TryGet("Steam_1", out var position));
        Assert.AreEqual(10, position.X);
        Assert.AreEqual(20, position.Y);
        Assert.AreEqual(30, position.Z);
    }

    [TestMethod]
    public void SerializesCachedLocationWithTakaroPayloadShape()
    {
        var cache = new LocationSnapshotCache();
        var player = new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null);
        cache.Store(player, new TakaroPosition(1.25, 2.5, 3.75, "valheim"), DateTimeOffset.UtcNow);

        Assert.IsTrue(cache.TryGet("Odin", out var position));
        using var document = JsonDocument.Parse(TakaroProtocol.CreateResponse("location", TakaroActionResult.Ok(position)));
        var payload = document.RootElement.GetProperty("payload");

        Assert.AreEqual(1.25, payload.GetProperty("x").GetDouble());
        Assert.AreEqual(2.5, payload.GetProperty("y").GetDouble());
        Assert.AreEqual(3.75, payload.GetProperty("z").GetDouble());
        Assert.AreEqual("valheim", payload.GetProperty("dimension").GetString());
    }
}
