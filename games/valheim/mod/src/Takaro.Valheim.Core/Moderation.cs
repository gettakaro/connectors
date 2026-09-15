using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public sealed record ValheimBan(
    string GameId,
    string Name,
    string? SteamId = null,
    string? PlatformId = null);

public static class ModerationFactory
{
    public static object[] CreateBanEntries(IEnumerable<ValheimBan> bans) =>
        bans
            .Where(ban => !string.IsNullOrWhiteSpace(ban.GameId))
            .Select(ban => new BanEntry(new BannedPlayer(ban.GameId, DisplayName(ban)), string.Empty, null))
            .ToArray();

    public static bool BanMatches(ValheimBan ban, string? identifier)
    {
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return false;
        }

        var needle = identifier!.Trim();
        return Matches(ban.GameId, needle)
            || Matches(ban.Name, needle)
            || Matches(ban.SteamId, needle)
            || Matches(ban.PlatformId, needle);
    }

    private static string DisplayName(ValheimBan ban) =>
        !string.IsNullOrWhiteSpace(ban.Name) ? ban.Name : ban.GameId;

    private static bool Matches(string? value, string needle) =>
        value is not null && value.Equals(needle, StringComparison.OrdinalIgnoreCase);

    private sealed record BanEntry(
        [property: JsonPropertyName("player")] BannedPlayer Player,
        [property: JsonPropertyName("reason")] string Reason,
        [property: JsonPropertyName("expiresAt")] DateTimeOffset? ExpiresAt);

    private sealed record BannedPlayer(
        [property: JsonPropertyName("gameId")] string GameId,
        [property: JsonPropertyName("name")] string Name);
}
