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
