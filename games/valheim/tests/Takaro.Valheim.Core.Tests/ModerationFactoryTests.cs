using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ModerationFactoryTests
{
    [TestMethod]
    public void BanEntriesUseOfficialTakaroArrayShape()
    {
        var entries = ModerationFactory.CreateBanEntries(new[]
        {
            new ValheimBan("Steam_76561198000735875", "Odin")
        });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var payload = document.RootElement.GetProperty("payload");

        Assert.AreEqual(JsonValueKind.Array, payload.ValueKind);
        Assert.AreEqual("Steam_76561198000735875", payload[0].GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Odin", payload[0].GetProperty("player").GetProperty("name").GetString());
        Assert.AreEqual("", payload[0].GetProperty("reason").GetString());
        Assert.AreEqual(JsonValueKind.Null, payload[0].GetProperty("expiresAt").ValueKind);
    }

    [TestMethod]
    public void BanEntriesCarryTakaroPlatformIdentifiers()
    {
        var entries = ModerationFactory.CreateBanEntries(new[]
        {
            new ValheimBan("Steam_76561198000735875", "Odin"),
            new ValheimBan("Xbox_2535405290924481", "Thor"),
            new ValheimBan("PlayStation_6151790232542195830", "Loki")
        });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var payload = document.RootElement.GetProperty("payload");

        var steam = payload[0].GetProperty("player");
        Assert.AreEqual("76561198000735875", steam.GetProperty("steamId").GetString());
        Assert.AreEqual("steam:76561198000735875", steam.GetProperty("platformId").GetString());
        Assert.IsFalse(steam.TryGetProperty("xboxLiveId", out _));

        var xbox = payload[1].GetProperty("player");
        Assert.AreEqual("2535405290924481", xbox.GetProperty("xboxLiveId").GetString());
        Assert.AreEqual("xbox:2535405290924481", xbox.GetProperty("platformId").GetString());
        Assert.IsFalse(xbox.TryGetProperty("steamId", out _));

        var psn = payload[2].GetProperty("player");
        Assert.AreEqual("psn:6151790232542195830", psn.GetProperty("platformId").GetString());
    }

    [TestMethod]
    public void BareSteamIdBanUsesThePlayersGameId()
    {
        // `ban <name>` on a connected Steam player stores the bare SteamID64 (the socket host name).
        var entries = ModerationFactory.CreateBanEntries(new[] { new ValheimBan("76561198000735875", "Odin") });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var player = document.RootElement.GetProperty("payload")[0].GetProperty("player");

        Assert.AreEqual("Steam_76561198000735875", player.GetProperty("gameId").GetString());
        Assert.AreEqual("76561198000735875", player.GetProperty("steamId").GetString());
        Assert.AreEqual("steam:76561198000735875", player.GetProperty("platformId").GetString());
    }

    [TestMethod]
    public void NameBanCarriesNoPlatformIdentifiers()
    {
        var entries = ModerationFactory.CreateBanEntries(new[] { new ValheimBan("Odin", "Odin") });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var player = document.RootElement.GetProperty("payload")[0].GetProperty("player");

        Assert.AreEqual("Odin", player.GetProperty("gameId").GetString());
        Assert.IsFalse(player.TryGetProperty("steamId", out _));
        Assert.IsFalse(player.TryGetProperty("xboxLiveId", out _));
        Assert.IsFalse(player.TryGetProperty("platformId", out _));
    }

    [TestMethod]
    public void BanAliasesMatchTakaroIdentifiersCaseInsensitively()
    {
        var ban = new ValheimBan("Steam_76561198000735875", "Odin", "76561198000735875", "steam:76561198000735875");

        Assert.IsTrue(ModerationFactory.BanMatches(ban, "Steam_76561198000735875"));
        Assert.IsTrue(ModerationFactory.BanMatches(ban, "odin"));
        Assert.IsTrue(ModerationFactory.BanMatches(ban, "76561198000735875"));
        Assert.IsTrue(ModerationFactory.BanMatches(ban, "steam:76561198000735875"));
        Assert.IsFalse(ModerationFactory.BanMatches(ban, "Thor"));
    }
}
