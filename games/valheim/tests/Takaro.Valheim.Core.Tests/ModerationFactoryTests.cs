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

    [TestMethod]
    public void BanWithoutARealNameIsLeftOut()
    {
        // Takaro writes every listBans name onto the player, and a null or missing name fails the
        // whole list, so an id-named or nameless ban must not be sent at all.
        var entries = ModerationFactory.CreateBanEntries(new[]
        {
            new ValheimBan("Steam_76561198000735875", "Steam_76561198000735875"),
            new ValheimBan("76561198000735876", "76561198000735876"),
            new ValheimBan("76561198000735877", "Steam_76561198000735877"),
            new ValheimBan("Steam_76561198000735878", "steam:76561198000735878"),
            new ValheimBan("Xbox_2535405290924481", "2535405290924481"),
            new ValheimBan("PlayStation_6151790232542195830", "PlayStation_6151790232542195830"),
            new ValheimBan("Steam_76561198000735879", ""),
            new ValheimBan("Steam_76561198000735880", "   "),
            new ValheimBan("Steam_76561198000735881", "Hehe")
        });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var payload = document.RootElement.GetProperty("payload");

        Assert.AreEqual(1, payload.GetArrayLength());
        Assert.AreEqual("Steam_76561198000735881", payload[0].GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Hehe", payload[0].GetProperty("player").GetProperty("name").GetString());
    }

    [TestMethod]
    public void NoBanEntryIsNamedAfterAnId()
    {
        var bans = new[]
        {
            new ValheimBan("Steam_76561198000735875", "Steam_76561198000735875"),
            new ValheimBan("Steam_76561198000735876", "Odin"),
            new ValheimBan("Odin", "Odin")
        };

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(ModerationFactory.CreateBanEntries(bans)));
        using var document = JsonDocument.Parse(response);

        foreach (var entry in document.RootElement.GetProperty("payload").EnumerateArray())
        {
            var player = entry.GetProperty("player");
            var name = player.GetProperty("name").GetString();
            Assert.IsFalse(string.IsNullOrWhiteSpace(name));
            if (player.TryGetProperty("platformId", out var platformId))
            {
                Assert.AreNotEqual(player.GetProperty("gameId").GetString(), name);
                Assert.AreNotEqual(platformId.GetString(), name);
            }
        }
    }

    [TestMethod]
    public void PickPlayerNameSkipsIdsAndBlanksForTheStoredName()
    {
        // An offline ban resolves no player or peer; the name must come from the known-players store.
        Assert.AreEqual(
            "Hehe",
            ModerationFactory.PickPlayerName("Steam_76561198000735875", null, "", "Steam_76561198000735875", "76561198000735875", "Hehe"));
        Assert.AreEqual("Odin", ModerationFactory.PickPlayerName("Steam_76561198000735875", " Odin ", "Hehe"));
        Assert.IsNull(ModerationFactory.PickPlayerName("Steam_76561198000735875", null, "Steam_76561198000735875", "steam:76561198000735875"));
        Assert.IsNull(ModerationFactory.PickPlayerName("76561198000735875", "Steam_76561198000735875"));
    }

    [TestMethod]
    public void NameBanKeepsItsOwnTextAsTheName()
    {
        Assert.IsFalse(ModerationFactory.IsPlaceholderName("Odin", "Odin"));
        Assert.IsTrue(ModerationFactory.IsPlaceholderName("Odin", ""));
    }
}
