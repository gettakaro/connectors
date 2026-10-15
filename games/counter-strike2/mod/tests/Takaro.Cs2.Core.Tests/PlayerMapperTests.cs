using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class PlayerMapperTests
{
    [TestMethod]
    public void MapsSteamId64ToTakaroPlayerShape()
    {
        var player = new Cs2Player(
            Name: "Major Tom",
            SteamId64: 76561198123456789,
            Ip: "192.0.2.10",
            Ping: 42,
            IsBot: false);

        var mapped = PlayerMapper.ToTakaroPlayer(player);

        Assert.AreEqual("76561198123456789", mapped.GameId);
        Assert.AreEqual("Major Tom", mapped.Name);
        Assert.AreEqual("76561198123456789", mapped.SteamId);
        Assert.AreEqual("steam:76561198123456789", mapped.PlatformId);
        Assert.AreEqual("192.0.2.10", mapped.Ip);
        Assert.AreEqual(42, mapped.Ping);
    }

    [TestMethod]
    public void FindMatchesGameIdPlatformIdSteamIdOrName()
    {
        var players = new[]
        {
            PlayerMapper.ToTakaroPlayer(new Cs2Player("Alpha", 76561198000000001, null, null, false)),
            PlayerMapper.ToTakaroPlayer(new Cs2Player("Bravo", 76561198000000002, null, null, false))
        };

        Assert.AreEqual("Alpha", PlayerMapper.Find(players, "76561198000000001")?.Name);
        Assert.AreEqual("Bravo", PlayerMapper.Find(players, "steam:76561198000000002")?.Name);
        Assert.AreEqual("Bravo", PlayerMapper.Find(players, "bravo")?.Name);
    }
}
