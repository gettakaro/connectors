using System.Text.RegularExpressions;
using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public sealed record ValheimPlayer(
    string Name,
    string PlatformUserId,
    string? SteamId,
    string? Ip,
    int? Ping);

public sealed record TakaroPlayer(
    [property: JsonPropertyName("gameId")] string GameId,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("steamId")] string? SteamId,
    [property: JsonPropertyName("platformId")] string? PlatformId,
    [property: JsonPropertyName("ip")] string? Ip,
    [property: JsonPropertyName("ping")] int? Ping);

public static class PlayerMapper
{
    private static readonly Regex SteamIdPattern = new(@"(?<steamId>7656119\d{10})", RegexOptions.Compiled);
    private static readonly Regex PlatformIdSegmentDisallowedCharacters = new(@"[^A-Za-z0-9_-]", RegexOptions.Compiled);

    public static TakaroPlayer ToTakaroPlayer(ValheimPlayer player)
    {
        var steamId = FirstNonEmpty(player.SteamId, ExtractSteamId(player.PlatformUserId));
        return new TakaroPlayer(
            GameId: player.PlatformUserId,
            Name: player.Name,
            SteamId: steamId,
            PlatformId: ToPlatformId(player.PlatformUserId, steamId),
            Ip: player.Ip,
            Ping: player.Ping);
    }

    public static TakaroPlayer? Find(IEnumerable<TakaroPlayer> players, string? identifier)
    {
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return null;
        }

        var needle = identifier!.Trim();
        return players.FirstOrDefault(player =>
            Matches(player.GameId, needle)
            || Matches(player.PlatformId, needle)
            || Matches(player.SteamId, needle)
            || Matches(player.Name, needle));
    }

    public static TakaroPlayer? FindUnique(IEnumerable<TakaroPlayer> players, string? identifier)
    {
        return TryFindUnique(players, identifier, out var player, out _)
            ? player
            : null;
    }

    public static bool TryFindUnique(
        IEnumerable<TakaroPlayer> players,
        string? identifier,
        out TakaroPlayer? player,
        out bool ambiguous)
    {
        if (players is null)
        {
            throw new ArgumentNullException(nameof(players));
        }

        player = null;
        ambiguous = false;
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return false;
        }

        var needle = identifier!.Trim();
        var candidates = players.ToArray();
        var stableMatches = candidates
            .Where(player =>
                Matches(player.GameId, needle)
                || Matches(player.PlatformId, needle)
                || Matches(player.SteamId, needle))
            .Take(2)
            .ToArray();
        if (stableMatches.Length > 0)
        {
            ambiguous = stableMatches.Length > 1;
            player = ambiguous ? null : stableMatches[0];
            return !ambiguous;
        }

        var nameMatches = candidates
            .Where(player => Matches(player.Name, needle))
            .Take(2)
            .ToArray();
        ambiguous = nameMatches.Length > 1;
        player = nameMatches.Length == 1 ? nameMatches[0] : null;
        return player is not null;
    }

    private static string? ToPlatformId(string platformUserId, string? steamId)
    {
        if (!string.IsNullOrWhiteSpace(steamId))
        {
            return $"steam:{steamId}";
        }

        if (platformUserId.StartsWith("Crossplay_", StringComparison.OrdinalIgnoreCase))
        {
            return $"crossplay:{NormalizePlatformIdSegment(platformUserId)}";
        }

        return $"valheim:{NormalizePlatformIdSegment(platformUserId)}";
    }

    private static string? ExtractSteamId(string value)
    {
        var match = SteamIdPattern.Match(value);
        return match.Success ? match.Groups["steamId"].Value : null;
    }

    private static string? FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value));

    private static string NormalizePlatformIdSegment(string value)
    {
        var normalized = PlatformIdSegmentDisallowedCharacters.Replace(value.Trim(), "_").Trim('_');
        return string.IsNullOrWhiteSpace(normalized) ? "unknown" : normalized;
    }

    private static bool Matches(string? value, string needle) =>
        value is not null && value.Equals(needle, StringComparison.OrdinalIgnoreCase);
}
