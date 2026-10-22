using System.Text.Json.Serialization;

namespace Takaro.Cs2.Core;

public static class EventFactory
{
    public static object PlayerConnected(TakaroPlayer player) => new PlayerLifecycle(player, DateTimeOffset.UtcNow);

    public static object PlayerDisconnected(TakaroPlayer player, string? reason = null) =>
        new PlayerLifecycle(player, DateTimeOffset.UtcNow);

    public static object ChatMessage(TakaroPlayer player, string message, bool teamOnly) =>
        new ChatMessageEvent(player, teamOnly ? "team" : "global", DateTimeOffset.UtcNow, message);

    public static object PlayerDeath(TakaroPlayer player, TakaroPlayer? attacker, string? weapon, bool headshot, TakaroPosition? position = null)
    {
        var data = new Dictionary<string, object?>
        {
            ["player"] = player,
            ["timestamp"] = DateTimeOffset.UtcNow
        };
        if (attacker is not null) data["attacker"] = attacker;
        if (position is not null) data["position"] = position;
        if (!string.IsNullOrWhiteSpace(weapon)) data["msg"] = $"killed with {weapon}" + (headshot ? " (headshot)" : "");
        return data;
    }

    public static object EntityKilled(TakaroPlayer player, string entity, string weapon) =>
        new EntityKilledEvent(player, entity, weapon, DateTimeOffset.UtcNow);

    public static object Log(string message, string level = "info") =>
        new LogEvent(message, DateTimeOffset.UtcNow);

    private sealed record PlayerLifecycle(
        [property: JsonPropertyName("player")] TakaroPlayer Player,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp);

    private sealed record ChatMessageEvent(
        [property: JsonPropertyName("player")] TakaroPlayer Player,
        [property: JsonPropertyName("channel")] string Channel,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp,
        [property: JsonPropertyName("msg")] string Msg);

    private sealed record EntityKilledEvent(
        [property: JsonPropertyName("player")] TakaroPlayer Player,
        [property: JsonPropertyName("entity")] string Entity,
        [property: JsonPropertyName("weapon")] string Weapon,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp);

    private sealed record LogEvent(
        [property: JsonPropertyName("msg")] string Msg,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp);
}
