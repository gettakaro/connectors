namespace Takaro.Valheim.Core;

public sealed class PlayerPositionCache
{
    private readonly TimeSpan freshness;
    private readonly Dictionary<string, Observation> observations = new(StringComparer.OrdinalIgnoreCase);
    private readonly object syncRoot = new();

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

        var observation = new Observation(position, observedAt);
        lock (syncRoot)
        {
            foreach (var alias in aliases)
            {
                observations[alias] = observation;
            }
        }

        return true;
    }

    public bool TryGet(string? playerIdentifier, DateTimeOffset now, out TakaroPosition position)
    {
        if (string.IsNullOrWhiteSpace(playerIdentifier))
        {
            position = default!;
            return false;
        }

        lock (syncRoot)
        {
            if (!observations.TryGetValue(playerIdentifier!.Trim(), out var observation))
            {
                position = default!;
                return false;
            }

            if (now - observation.ObservedAt > freshness)
            {
                var expiredAliases = new List<string>();
                foreach (var entry in observations)
                {
                    if (ReferenceEquals(entry.Value, observation))
                    {
                        expiredAliases.Add(entry.Key);
                    }
                }

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
