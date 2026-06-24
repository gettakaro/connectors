using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PlayerMapperTests
{
    [TestMethod]
    public void ToTakaroPlayerUsesPlatformIdAsStableGameId()
    {
        var player = new ValheimPlayer(
            Name: "Eikthyr Hunter",
            PlatformUserId: "Steam_76561198000000001",
            SteamId: "76561198000000001",
            Ip: "127.0.0.1",
            Ping: 42);

        var takaro = PlayerMapper.ToTakaroPlayer(player);

        Assert.AreEqual("Steam_76561198000000001", takaro.GameId);
        Assert.AreEqual("Eikthyr Hunter", takaro.Name);
        Assert.AreEqual("76561198000000001", takaro.SteamId);
        Assert.AreEqual("steam:76561198000000001", takaro.PlatformId);
        Assert.AreEqual("127.0.0.1", takaro.Ip);
        Assert.AreEqual(42, takaro.Ping);
    }

    [TestMethod]
    public void ToTakaroPlayerUsesSteamPlatformIdWhenSteamIdIsEmbedded()
    {
        var player = new ValheimPlayer(
            Name: "Odin",
            PlatformUserId: "Steam_76561198000735875",
            SteamId: null,
            Ip: null,
            Ping: null);

        var takaro = PlayerMapper.ToTakaroPlayer(player);

        Assert.AreEqual("Steam_76561198000735875", takaro.GameId);
        Assert.AreEqual("76561198000735875", takaro.SteamId);
        Assert.AreEqual("steam:76561198000735875", takaro.PlatformId);
    }

    [TestMethod]
    public void ToTakaroPlayerNormalizesValheimFallbackPlatformId()
    {
        var player = new ValheimPlayer(
            Name: "Odin",
            PlatformUserId: "-977956709:3",
            SteamId: null,
            Ip: null,
            Ping: null);

        var takaro = PlayerMapper.ToTakaroPlayer(player);

        Assert.AreEqual("-977956709:3", takaro.GameId);
        Assert.AreEqual("valheim:-977956709_3", takaro.PlatformId);
    }

    [TestMethod]
    public void FindsPlayerByGameIdPlatformIdSteamIdOrName()
    {
        var players = new[]
        {
            new TakaroPlayer("Steam_76561198000000001", "Eikthyr Hunter", "76561198000000001", "steam:76561198000000001", null, null),
            new TakaroPlayer("Crossplay_abc", "Boar Tamer", null, "crossplay:Crossplay_abc", null, null)
        };

        Assert.AreEqual("Eikthyr Hunter", PlayerMapper.Find(players, "Steam_76561198000000001")?.Name);
        Assert.AreEqual("Eikthyr Hunter", PlayerMapper.Find(players, "steam:76561198000000001")?.Name);
        Assert.AreEqual("Eikthyr Hunter", PlayerMapper.Find(players, "76561198000000001")?.Name);
        Assert.AreEqual("Boar Tamer", PlayerMapper.Find(players, "boar tamer")?.Name);
    }
}
