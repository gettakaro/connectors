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

    public static object CompanionPlayerDeath(
        TakaroPlayer player,
        DateTimeOffset timestamp,
        TakaroPosition position,
        string? causeHint,
        string? attackerCodeHint)
    {
        var data = new Dictionary<string, object?>
        {
            ["player"] = player,
            ["timestamp"] = timestamp,
            ["position"] = position
        };
        var hints = new List<string>(capacity: 2);
        if (!string.IsNullOrWhiteSpace(causeHint))
        {
            hints.Add(causeHint!.Trim());
        }

        if (!string.IsNullOrWhiteSpace(attackerCodeHint))
        {
            hints.Add($"attacker: {attackerCodeHint!.Trim()}");
        }

        if (hints.Count > 0)
        {
            data["msg"] = string.Join("; ", hints);
        }

        return data;
    }

    public static object EntityKilled(
        TakaroPlayer player,
        string entity,
        DateTimeOffset timestamp,
        string weapon)
    {
        var data = new Dictionary<string, object?>
        {
            ["player"] = player,
            ["entity"] = entity.Trim(),
            ["timestamp"] = timestamp,
            ["weapon"] = weapon.Trim()
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

public sealed class PlayerLifecyclePresenceFilter
{
    private readonly HashSet<string> admittedGameIds = new(StringComparer.OrdinalIgnoreCase);

    public IReadOnlyList<TakaroPlayer> SelectTrackable(
        IReadOnlyCollection<TakaroPlayer> onlinePlayers,
        IReadOnlyCollection<string> playersWithObservedPositions)
    {
        var onlineByGameId = onlinePlayers
            .GroupBy(player => player.GameId, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);

        admittedGameIds.IntersectWith(onlineByGameId.Keys);
        foreach (var gameId in playersWithObservedPositions)
        {
            if (onlineByGameId.ContainsKey(gameId))
            {
                admittedGameIds.Add(gameId);
            }
        }

        return onlineByGameId.Values
            .Where(player => admittedGameIds.Contains(player.GameId))
            .ToArray();
    }
}

public sealed class PlayerLifecyclePollCoordinator
{
    private readonly PlayerLifecyclePresenceFilter presence = new();
    private readonly PlayerLifecycleEventTracker lifecycle = new();

    public IReadOnlyList<TakaroPlayerLifecycleEvent> Update(
        IReadOnlyCollection<TakaroPlayer> onlinePlayers,
        IReadOnlyCollection<string> playersWithObservedPositions,
        DateTimeOffset timestamp)
    {
        var trackablePlayers = presence.SelectTrackable(
            onlinePlayers,
            playersWithObservedPositions);
        return lifecycle.Update(trackablePlayers, timestamp);
    }
}

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
