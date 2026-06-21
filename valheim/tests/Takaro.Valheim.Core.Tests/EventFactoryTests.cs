using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class EventFactoryTests
{
    [TestMethod]
    public void ChatMessageUsesTakaroPayloadShape()
    {
        var player = new TakaroPlayer(
            GameId: "Steam_76561198000735875",
            Name: "Odin",
            SteamId: "76561198000735875",
            PlatformId: "steam:76561198000735875",
            Ip: null,
            Ping: null);
        var timestamp = new DateTimeOffset(2026, 6, 20, 19, 30, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "chat-message",
            EventFactory.ChatMessage(player, "global", timestamp, "hello from game"));
        using var document = JsonDocument.Parse(json);
        var payload = document.RootElement.GetProperty("payload");
        var data = payload.GetProperty("data");

        Assert.AreEqual("gameEvent", document.RootElement.GetProperty("type").GetString());
        Assert.AreEqual("chat-message", payload.GetProperty("type").GetString());
        Assert.AreEqual("hello from game", data.GetProperty("msg").GetString());
        Assert.AreEqual("global", data.GetProperty("channel").GetString());
        Assert.AreEqual("2026-06-20T19:30:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual("Steam_76561198000735875", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
    }

    [TestMethod]
    public void PlayerConnectedUsesTakaroPayloadShape()
    {
        var player = Player("Steam_76561198000735875", "Odin");
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 0, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "player-connected",
            EventFactory.PlayerConnected(player, timestamp));
        using var document = JsonDocument.Parse(json);
        var payload = document.RootElement.GetProperty("payload");
        var data = payload.GetProperty("data");

        Assert.AreEqual("gameEvent", document.RootElement.GetProperty("type").GetString());
        Assert.AreEqual("player-connected", payload.GetProperty("type").GetString());
        Assert.AreEqual("2026-06-21T08:00:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual("Steam_76561198000735875", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
    }

    [TestMethod]
    public void PlayerDisconnectedUsesTakaroPayloadShape()
    {
        var player = Player("Steam_76561198000735875", "Odin");
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 1, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "player-disconnected",
            EventFactory.PlayerDisconnected(player, timestamp));
        using var document = JsonDocument.Parse(json);
        var payload = document.RootElement.GetProperty("payload");
        var data = payload.GetProperty("data");

        Assert.AreEqual("gameEvent", document.RootElement.GetProperty("type").GetString());
        Assert.AreEqual("player-disconnected", payload.GetProperty("type").GetString());
        Assert.AreEqual("2026-06-21T08:01:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual("Steam_76561198000735875", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
    }

    [TestMethod]
    public void PlayerDeathUsesTakaroPayloadShape()
    {
        var player = Player("Steam_76561198000735875", "Odin");
        var attacker = Player("Steam_76561198000000002", "Loki");
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 2, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "player-death",
            EventFactory.PlayerDeath(
                player,
                timestamp,
                new TakaroPosition(10, 35, -5, "valheim"),
                attacker,
                "SwordIron"));
        using var document = JsonDocument.Parse(json);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");

        Assert.AreEqual("player-death", document.RootElement.GetProperty("payload").GetProperty("type").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
        Assert.AreEqual("2026-06-21T08:02:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual(10, data.GetProperty("position").GetProperty("x").GetInt32());
        Assert.AreEqual("valheim", data.GetProperty("position").GetProperty("dimension").GetString());
        Assert.AreEqual("Loki", data.GetProperty("attacker").GetProperty("name").GetString());
        Assert.AreEqual("killed with SwordIron", data.GetProperty("msg").GetString());
        Assert.IsFalse(data.TryGetProperty("weapon", out _));
    }

    [TestMethod]
    public void PlayerDeathOmitsNullAttackerAndWeapon()
    {
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 3, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "player-death",
            EventFactory.PlayerDeath(
                Player("Steam_76561198000735875", "Odin"),
                timestamp,
                new TakaroPosition(1, 2, 3, "valheim"),
                attacker: null,
                weapon: null));
        using var document = JsonDocument.Parse(json);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");

        Assert.IsFalse(data.TryGetProperty("attacker", out _));
        Assert.IsFalse(data.TryGetProperty("weapon", out _));
        Assert.IsFalse(data.TryGetProperty("msg", out _));
    }

    [TestMethod]
    public void EntityKilledUsesTakaroPayloadShape()
    {
        var killer = Player("Steam_76561198000735875", "Odin");
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 4, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "entity-killed",
            EventFactory.EntityKilled(
                new TakaroEntity("Boar", "Boar"),
                timestamp,
                new TakaroPosition(4, 5, 6, "valheim"),
                killer,
                "Club"));
        using var document = JsonDocument.Parse(json);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");

        Assert.AreEqual("entity-killed", document.RootElement.GetProperty("payload").GetProperty("type").GetString());
        Assert.AreEqual("Boar", data.GetProperty("entity").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
        Assert.AreEqual("Steam_76561198000735875", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("2026-06-21T08:04:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual("Club", data.GetProperty("weapon").GetString());
        Assert.IsFalse(data.TryGetProperty("killer", out _));
        Assert.IsFalse(data.TryGetProperty("position", out _));
    }

    [TestMethod]
    public void LogUsesTakaroPayloadShape()
    {
        var timestamp = new DateTimeOffset(2026, 6, 21, 8, 5, 0, TimeSpan.Zero);

        var json = TakaroProtocol.CreateGameEvent(
            "log",
            EventFactory.Log("info", "connector started", timestamp));
        using var document = JsonDocument.Parse(json);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");

        Assert.AreEqual("log", document.RootElement.GetProperty("payload").GetProperty("type").GetString());
        Assert.AreEqual("connector started", data.GetProperty("msg").GetString());
        Assert.AreEqual("2026-06-21T08:05:00+00:00", data.GetProperty("timestamp").GetString());
        Assert.IsFalse(data.TryGetProperty("level", out _));
    }

    private static TakaroPlayer Player(string gameId, string name) =>
        new(
            GameId: gameId,
            Name: name,
            SteamId: "76561198000735875",
            PlatformId: "steam:76561198000735875",
            Ip: null,
            Ping: null);
}
