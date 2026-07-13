namespace Takaro.Valheim.Core;

public static class CompanionInventoryActionPolicy
{
    public static TakaroActionResult FromResolvedPlayer(
        CompanionMode mode,
        TakaroPlayer? player,
        CompanionInventoryCache cache,
        DateTimeOffset now)
    {
        if (cache is null)
        {
            throw new ArgumentNullException(nameof(cache));
        }

        if (mode == CompanionMode.Disabled)
        {
            return TakaroActionResult.Error(
                "player_component_unavailable",
                "Valheim companion inventory reporting is disabled.");
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

        foreach (var alias in aliases)
        {
            if (cache.TryGetStable(alias, now, out var items) == CompanionInventoryState.Fresh)
            {
                return TakaroActionResult.Ok(items);
            }
        }

        return TakaroActionResult.Error(
            "player_component_unavailable",
            $"Valheim player '{player.GameId}' has no fresh companion inventory snapshot.");
    }
}
