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

    /// <summary>
    /// A player counts as present once the server has a position and can read the character.
    /// Takaro picks up a character switch from player-connected, so that event has to name it.
    /// </summary>
    public static bool IsObserved(TakaroPlayer player, bool hasPosition) =>
        hasPosition && player.Character is not null;

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
            // A relog onto another character between two polls still has to reach Takaro as one.
            if (previousPlayers.TryGetValue(player.GameId, out var previous) && SwitchedCharacter(previous, player))
            {
                events.Add(new TakaroPlayerLifecycleEvent(
                    "player-disconnected",
                    previous,
                    EventFactory.PlayerDisconnected(previous, timestamp)));
                events.Add(new TakaroPlayerLifecycleEvent(
                    "player-connected",
                    player,
                    EventFactory.PlayerConnected(player, timestamp)));
            }

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

    private static bool SwitchedCharacter(TakaroPlayer previous, TakaroPlayer current) =>
        previous.Character is not null
        && current.Character is not null
        && !string.Equals(previous.Character.Id, current.Character.Id, StringComparison.Ordinal);

    // The character is unreadable for a moment around a respawn; the last known one still holds.
    private void ReplaceSnapshot(Dictionary<string, TakaroPlayer> players)
    {
        var carried = players.ToDictionary(
            entry => entry.Key,
            entry => entry.Value.Character is null && previousPlayers.TryGetValue(entry.Key, out var previous)
                ? entry.Value with { Character = previous.Character }
                : entry.Value,
            StringComparer.OrdinalIgnoreCase);
        previousPlayers.Clear();
        foreach (var entry in carried)
        {
            previousPlayers[entry.Key] = entry.Value;
        }
    }
}
