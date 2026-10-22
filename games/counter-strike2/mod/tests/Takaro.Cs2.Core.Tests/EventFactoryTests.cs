using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class EventFactoryTests
{
    [TestMethod]
    public void EntityKilledCarriesAuthenticatedKillerAndDisplayNames()
    {
        var player = new TakaroPlayer("76561198000000001", "Limon", "76561198000000001", "steam:76561198000000001", null, null);
        var json = System.Text.Json.JsonSerializer.SerializeToElement(EventFactory.EntityKilled(player, "Chicken", "AK-47"));
        Assert.AreEqual("76561198000000001", json.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Chicken", json.GetProperty("entity").GetString());
        Assert.AreEqual("AK-47", json.GetProperty("weapon").GetString());
    }
    [TestMethod]
    public void CreatesChatMessagePayload()
    {
        var player = PlayerMapper.ToTakaroPlayer(new Cs2Player("Sender", 76561198000000001, null, null, false));

        var data = JsonSerializer.SerializeToElement(EventFactory.ChatMessage(player, "go long", teamOnly: true));

        Assert.AreEqual("go long", data.GetProperty("msg").GetString());
        Assert.AreEqual("team", data.GetProperty("channel").GetString());
        Assert.IsTrue(data.TryGetProperty("timestamp", out _));
        Assert.AreEqual("Sender", data.GetProperty("player").GetProperty("name").GetString());
        Assert.IsFalse(data.GetProperty("player").TryGetProperty("online", out _));
    }

    [TestMethod]
    public void CreatesPlayerDeathPayloadWithOptionalAttacker()
    {
        var victim = PlayerMapper.ToTakaroPlayer(new Cs2Player("Victim", 76561198000000001, null, null, false));
        var attacker = PlayerMapper.ToTakaroPlayer(new Cs2Player("Attacker", 76561198000000002, null, null, false));

        var data = JsonSerializer.SerializeToElement(EventFactory.PlayerDeath(victim, attacker, "ak47", headshot: true));

        Assert.AreEqual("Victim", data.GetProperty("player").GetProperty("name").GetString());
        Assert.AreEqual("Attacker", data.GetProperty("attacker").GetProperty("name").GetString());
        Assert.AreEqual("killed with ak47 (headshot)", data.GetProperty("msg").GetString());
        Assert.IsFalse(data.TryGetProperty("headshot", out _));
    }
}
