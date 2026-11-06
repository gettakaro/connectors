using System.Collections.Concurrent;

namespace Takaro.Cs2.Core;

/// <summary>
/// Caches each player's CS2 profile rank. The client reports it to the server when joining, and
/// Valve's official matches are what change it, so the source is declared as client-reported.
/// Takaro polls this on every player sync, so reads never touch the game thread.
/// </summary>
public sealed class Cs2Progression
{
    private readonly ConcurrentDictionary<string, int> ranks = new(StringComparer.Ordinal);

    public void Observe(Cs2Player player)
    {
        if (!player.IsBot && player.ProfileRank > 0) ranks[player.SteamId64.ToString()] = player.ProfileRank;
    }

    /// <returns>The tracks; empty for a known player with no rank yet; null for an unknown player.</returns>
    public object? Read(string gameId, bool knownPlayer)
    {
        if (ranks.TryGetValue(gameId, out var rank)) return new[] { new { key = "character", name = "Profile Rank", level = rank } };
        return knownPlayer ? Array.Empty<object>() : null;
    }
}
