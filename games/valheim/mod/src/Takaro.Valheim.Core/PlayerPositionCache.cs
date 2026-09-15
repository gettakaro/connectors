namespace Takaro.Valheim.Core;

public sealed class PlayerPositionCache
{
    private readonly TimeSpan freshness;
    private readonly Dictionary<string, Observation> observations = new(StringComparer.OrdinalIgnoreCase);
    private readonly object syncRoot = new();
    private object? currentWorld;
    private bool hasCurrentWorld;

    public PlayerPositionCache(TimeSpan freshness)
    {
        if (freshness <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(freshness), "Position freshness must be positive.");
        }

        this.freshness = freshness;
    }

    public bool Remember(TakaroPlayer player, TakaroPosition position, DateTimeOffset observedAt)
    {
        if (!IsRealPosition(position))
        {
            return false;
        }

        var aliases = PlayerAliases(player).ToArray();
        if (aliases.Length == 0)
        {
            return false;
        }

        lock (syncRoot)
        {
            RememberLocked(aliases, position, observedAt);
        }

        return true;
    }

    public void SwitchWorld(object? worldIdentity)
    {
        lock (syncRoot)
        {
            if (hasCurrentWorld && ReferenceEquals(currentWorld, worldIdentity))
            {
                return;
            }

            observations.Clear();
            currentWorld = worldIdentity;
            hasCurrentWorld = true;
        }
    }

    public bool RememberIfCurrentWorld(
        object? worldIdentity,
        TakaroPlayer player,
        TakaroPosition position,
        DateTimeOffset observedAt)
    {
        if (!IsRealPosition(position))
        {
            return false;
        }

        var aliases = PlayerAliases(player).ToArray();
        if (aliases.Length == 0)
        {
            return false;
        }

        lock (syncRoot)
        {
            if (!hasCurrentWorld || !ReferenceEquals(currentWorld, worldIdentity))
            {
                return false;
            }

            RememberLocked(aliases, position, observedAt);
            return true;
        }
    }

    public bool TryGetForCurrentWorld(
        object? worldIdentity,
        string? playerIdentifier,
        DateTimeOffset now,
        out TakaroPosition position)
    {
        lock (syncRoot)
        {
            if (!hasCurrentWorld || !ReferenceEquals(currentWorld, worldIdentity))
            {
                position = default!;
                return false;
            }

            return TryGetLocked(playerIdentifier, now, out position);
        }
    }

    public bool TryGet(string? playerIdentifier, DateTimeOffset now, out TakaroPosition position)
    {
        lock (syncRoot)
        {
            return TryGetLocked(playerIdentifier, now, out position);
        }
    }

    private void RememberLocked(
        IReadOnlyList<string> aliases,
        TakaroPosition position,
        DateTimeOffset observedAt)
    {
        var observation = new Observation(position, observedAt);
        foreach (var alias in aliases)
        {
            observations[alias] = observation;
        }
    }

    private bool TryGetLocked(string? playerIdentifier, DateTimeOffset now, out TakaroPosition position)
    {
        if (string.IsNullOrWhiteSpace(playerIdentifier)
            || !observations.TryGetValue(playerIdentifier!.Trim(), out var observation))
        {
            position = default!;
            return false;
        }

        if (now - observation.ObservedAt > freshness)
        {
            var expiredAliases = observations
                .Where(entry => ReferenceEquals(entry.Value, observation))
                .Select(entry => entry.Key)
                .ToArray();
            foreach (var alias in expiredAliases)
            {
                observations.Remove(alias);
            }

            position = default!;
            return false;
        }

        position = observation.Position;
        return true;
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            observations.Clear();
        }
    }

    private static IEnumerable<string> PlayerAliases(TakaroPlayer player) =>
        new[] { player.GameId, player.PlatformId, player.SteamId, player.Name }
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!.Trim())
            .Distinct(StringComparer.OrdinalIgnoreCase);

    private static bool IsRealPosition(TakaroPosition position) =>
        IsFinite(position.X)
        && IsFinite(position.Y)
        && IsFinite(position.Z)
        && (position.X != 0d || position.Y != 0d || position.Z != 0d);

    private static bool IsFinite(double value) =>
        !double.IsNaN(value) && !double.IsInfinity(value);

    private sealed class Observation
    {
        public Observation(TakaroPosition position, DateTimeOffset observedAt)
        {
            Position = position;
            ObservedAt = observedAt;
        }

        public TakaroPosition Position { get; }

        public DateTimeOffset ObservedAt { get; }
    }
}
