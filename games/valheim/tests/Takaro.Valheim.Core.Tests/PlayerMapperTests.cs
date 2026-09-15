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

    [TestMethod]
    public void FindUniquePrefersOneStableMatchOverAnotherPlayersMatchingName()
    {
        var nameCollision = new TakaroPlayer("other", "target", "2", "steam:2", null, null);
        var stableMatch = new TakaroPlayer("target", "Stable Match", "1", "steam:1", null, null);

        var result = FindUnique(new[] { nameCollision, stableMatch }, "target");

        Assert.AreSame(stableMatch, result);
    }

    [TestMethod]
    public void FindUniqueRejectsDuplicateNames()
    {
        var players = new[]
        {
            new TakaroPlayer("game-1", "Shared Name", "1", "steam:1", null, null),
            new TakaroPlayer("game-2", "Shared Name", "2", "steam:2", null, null)
        };

        Assert.IsNull(FindUnique(players, "shared name"));
    }

    [TestMethod]
    public void FindUniqueRejectsDuplicateStableIdentifiers()
    {
        var players = new[]
        {
            new TakaroPlayer("duplicate", "First", "1", "steam:1", null, null),
            new TakaroPlayer("duplicate", "Second", "2", "steam:2", null, null)
        };

        Assert.IsNull(FindUnique(players, "duplicate"));
    }

    [TestMethod]
    public void FindUniqueAllowsOneMatchingNameWhenNoStableAliasMatches()
    {
        var expected = new TakaroPlayer("game-1", "Unique Name", "1", "steam:1", null, null);
        var players = new[]
        {
            expected,
            new TakaroPlayer("game-2", "Other Name", "2", "steam:2", null, null)
        };

        Assert.AreSame(expected, FindUnique(players, " unique name "));
    }

    [TestMethod]
    public void TryFindUniqueDistinguishesMissingResult()
    {
        var players = new[]
        {
            new TakaroPlayer("game-1", "First", "1", "steam:1", null, null)
        };

        var result = TryFindUnique(players, "absent");

        Assert.IsFalse(result.Found);
        Assert.IsNull(result.Player);
        Assert.IsFalse(result.Ambiguous);
    }

    [TestMethod]
    public void TryFindUniqueDistinguishesFoundResult()
    {
        var expected = new TakaroPlayer("target", "Target", "1", "steam:1", null, null);
        var nameCollision = new TakaroPlayer("other", "target", "2", "steam:2", null, null);

        var result = TryFindUnique(new[] { nameCollision, expected }, "target");

        Assert.IsTrue(result.Found);
        Assert.AreSame(expected, result.Player);
        Assert.IsFalse(result.Ambiguous);
    }

    [TestMethod]
    public void TryFindUniqueDistinguishesAmbiguousStableResult()
    {
        var players = new[]
        {
            new TakaroPlayer("duplicate", "First", "1", "steam:1", null, null),
            new TakaroPlayer("duplicate", "Second", "2", "steam:2", null, null)
        };

        var result = TryFindUnique(players, "duplicate");

        Assert.IsFalse(result.Found);
        Assert.IsNull(result.Player);
        Assert.IsTrue(result.Ambiguous);
    }

    [TestMethod]
    public void TryFindUniqueDistinguishesAmbiguousNameResult()
    {
        var players = new[]
        {
            new TakaroPlayer("game-1", "Shared", "1", "steam:1", null, null),
            new TakaroPlayer("game-2", "Shared", "2", "steam:2", null, null)
        };

        var result = TryFindUnique(players, "shared");

        Assert.IsFalse(result.Found);
        Assert.IsNull(result.Player);
        Assert.IsTrue(result.Ambiguous);
    }

    private static TakaroPlayer? FindUnique(IEnumerable<TakaroPlayer> players, string identifier)
    {
        var method = typeof(PlayerMapper).GetMethod("FindUnique")
            ?? throw new AssertFailedException("PlayerMapper is missing FindUnique.");

        return method.Invoke(null, new object?[] { players, identifier }) as TakaroPlayer;
    }

    private static (bool Found, TakaroPlayer? Player, bool Ambiguous) TryFindUnique(
        IEnumerable<TakaroPlayer> players,
        string identifier)
    {
        var method = typeof(PlayerMapper).GetMethods()
            .SingleOrDefault(candidate =>
                candidate.Name == "TryFindUnique"
                && candidate.GetParameters().Length == 4)
            ?? throw new AssertFailedException("PlayerMapper is missing ambiguity-aware TryFindUnique.");
        var arguments = new object?[] { players, identifier, null, false };
        var found = method.Invoke(null, arguments) as bool?
            ?? throw new AssertFailedException("PlayerMapper.TryFindUnique returned no boolean result.");

        return (found, arguments[2] as TakaroPlayer, arguments[3] as bool? ?? false);
    }
}
