using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public static class EventFactory
{
    public static object ChatMessage(TakaroPlayer player, string channel, DateTimeOffset timestamp, string message) =>
        new ChatMessageEvent(player, channel, timestamp, message);

    public static object PlayerConnected(TakaroPlayer player, DateTimeOffset timestamp) =>
        new PlayerLifecycleEventData(player, timestamp);

    public static object PlayerDisconnected(TakaroPlayer player, DateTimeOffset timestamp) =>
        new PlayerLifecycleEventData(player, timestamp);

    public static object PlayerDeath(
        TakaroPlayer player,
        DateTimeOffset timestamp,
        TakaroPosition position,
        TakaroPlayer? attacker,
        string? weapon)
    {
        var data = new Dictionary<string, object?>
        {
            ["player"] = player,
            ["timestamp"] = timestamp,
            ["position"] = position
        };

        if (attacker is not null)
        {
            data["attacker"] = attacker;
        }

        if (!string.IsNullOrWhiteSpace(weapon))
        {
            data["msg"] = $"killed with {weapon}";
        }

        return data;
    }

    public static object EntityKilled(
        TakaroEntity entity,
        DateTimeOffset timestamp,
        TakaroPosition position,
        TakaroPlayer? killer,
        string? weapon = null)
    {
        var data = new Dictionary<string, object?>
        {
            ["player"] = killer ?? new TakaroPlayer("unknown", "unknown", null, null, null, null),
            ["entity"] = entity.Code,
            ["timestamp"] = timestamp,
            ["weapon"] = string.IsNullOrWhiteSpace(weapon) ? "Unknown" : weapon
        };

        return data;
    }

    public static object Log(string level, string message, DateTimeOffset timestamp) =>
        new LogEventData(message, timestamp);

    private sealed record ChatMessageEvent(
        [property: JsonPropertyName("player")] TakaroPlayer Player,
        [property: JsonPropertyName("channel")] string Channel,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp,
        [property: JsonPropertyName("msg")] string Msg);

    private sealed record PlayerLifecycleEventData(
        [property: JsonPropertyName("player")] TakaroPlayer Player,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp);

    private sealed record LogEventData(
        [property: JsonPropertyName("msg")] string Msg,
        [property: JsonPropertyName("timestamp")] DateTimeOffset Timestamp);
}

public sealed record TakaroPlayerLifecycleEvent(
    string Type,
    TakaroPlayer Player,
    object Data);

public sealed class PlayerLifecycleEventTracker
{
    private readonly Dictionary<string, TakaroPlayer> previousPlayers = new(StringComparer.OrdinalIgnoreCase);
    private bool hasSnapshot;

    public IReadOnlyList<TakaroPlayerLifecycleEvent> Update(
        IReadOnlyCollection<TakaroPlayer> currentPlayers,
        DateTimeOffset timestamp)
    {
        var currentByGameId = currentPlayers
            .GroupBy(player => player.GameId, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);

        if (!hasSnapshot)
        {
            ReplaceSnapshot(currentByGameId);
            hasSnapshot = true;
            return Array.Empty<TakaroPlayerLifecycleEvent>();
        }

        var events = new List<TakaroPlayerLifecycleEvent>();
        foreach (var player in currentByGameId.Values.OrderBy(player => player.GameId, StringComparer.OrdinalIgnoreCase))
        {
            if (!previousPlayers.ContainsKey(player.GameId))
            {
                events.Add(new TakaroPlayerLifecycleEvent(
                    "player-connected",
                    player,
                    EventFactory.PlayerConnected(player, timestamp)));
            }
        }

        foreach (var player in previousPlayers.Values.OrderBy(player => player.GameId, StringComparer.OrdinalIgnoreCase))
        {
            if (!currentByGameId.ContainsKey(player.GameId))
            {
                events.Add(new TakaroPlayerLifecycleEvent(
                    "player-disconnected",
                    player,
                    EventFactory.PlayerDisconnected(player, timestamp)));
            }
        }

        ReplaceSnapshot(currentByGameId);
        return events;
    }

    private void ReplaceSnapshot(Dictionary<string, TakaroPlayer> players)
    {
        previousPlayers.Clear();
        foreach (var entry in players)
        {
            previousPlayers[entry.Key] = entry.Value;
        }
    }
}
