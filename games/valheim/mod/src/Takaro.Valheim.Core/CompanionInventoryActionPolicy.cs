namespace Takaro.Valheim.Core;

public static class CompanionInventoryActionPolicy
{
    public const string UnsupportedErrorCode = "server_only_unsupported";
    public const string StaleErrorCode = "inventory_snapshot_stale";

    /// <summary>
    /// Answers getPlayerInventory from the optional companion's snapshot. Without a fresh
    /// snapshot the answer is an error, never an empty list: Valheim keeps inventories on the
    /// game client, so the server cannot confirm that a player carries nothing.
    /// </summary>
    public static TakaroActionResult FromResolvedPlayer(
        TakaroPlayer? player,
        CompanionInventoryCache cache,
        DateTimeOffset now)
    {
        if (cache is null)
        {
            throw new ArgumentNullException(nameof(cache));
        }

        if (player is null)
        {
            return TakaroActionResult.Error(
                "player_not_found",
                "The requested Valheim player is not online.");
        }

        var aliases = new[] { player.GameId, player.PlatformId, player.SteamId }
            .Where(alias => !string.IsNullOrWhiteSpace(alias))
            .Select(alias => alias!)
            .Distinct(StringComparer.OrdinalIgnoreCase);

        var sawExpired = false;
        foreach (var alias in aliases)
        {
            switch (cache.TryGetStable(alias, now, out var items))
            {
                case CompanionInventoryState.Fresh:
                    return TakaroActionResult.Ok(items);
                case CompanionInventoryState.Expired:
                    sawExpired = true;
                    break;
            }
        }

        return sawExpired
            ? TakaroActionResult.Error(
                StaleErrorCode,
                $"Valheim player '{player.GameId}' has no fresh inventory snapshot from the optional inventory companion.")
            : TakaroActionResult.Error(
                UnsupportedErrorCode,
                "Valheim keeps player inventories on the game client; this player does not run the optional Takaro inventory companion.");
    }
}
