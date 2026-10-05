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
    [property: JsonPropertyName("ping")] int? Ping,
    [property: JsonPropertyName("xboxLiveId")] string? XboxLiveId = null);

/// <summary>
/// The Takaro identity fields derived from a Valheim <c>PlatformUserID</c> string
/// (<c>Steam_&lt;SteamID64&gt;</c>, <c>Xbox_&lt;XUID&gt;</c>, <c>PlayStation_&lt;id&gt;</c>, ...).
/// Takaro matches players only on these explicit fields and never parses <c>platformId</c>,
/// so Steam and Xbox players must carry <c>steamId</c> / <c>xboxLiveId</c> raw.
/// </summary>
public sealed record PlatformIdentity(string? SteamId, string? XboxLiveId, string PlatformId)
{
    /// <summary>True for a Steam, Xbox or PlayStation account; false for the crossplay/valheim fallbacks.</summary>
    public bool IsPlatformAccount =>
        SteamId is not null || XboxLiveId is not null || PlatformId.StartsWith("psn:", StringComparison.Ordinal);
}

public static class PlayerMapper
{
    private static readonly Regex SteamIdPattern = new(@"(?<steamId>7656119\d{10})", RegexOptions.Compiled);
    private static readonly Regex BareSteamIdPattern = new(@"^7656119\d{10}$", RegexOptions.Compiled);
    private static readonly Regex XuidPattern = new(@"^\d{1,20}$", RegexOptions.Compiled);
    private static readonly Regex PlatformIdSegmentPattern = new(@"^[A-Za-z0-9_-]+$", RegexOptions.Compiled);
    private static readonly Regex PlatformIdSegmentDisallowedCharacters = new(@"[^A-Za-z0-9_-]", RegexOptions.Compiled);

    public static TakaroPlayer ToTakaroPlayer(ValheimPlayer player)
    {
        var identity = ToIdentity(player.PlatformUserId, player.SteamId);
        return new TakaroPlayer(
            GameId: player.PlatformUserId,
            Name: player.Name,
            SteamId: identity.SteamId,
            PlatformId: identity.PlatformId,
            Ip: player.Ip,
            Ping: player.Ping,
            XboxLiveId: identity.XboxLiveId);
    }

    public static PlatformIdentity ToIdentity(string platformUserId, string? knownSteamId = null)
    {
        var raw = (platformUserId ?? string.Empty).Trim();
        var steamId = FirstNonEmpty(knownSteamId, ExtractSteamId(raw));
        if (!string.IsNullOrWhiteSpace(steamId))
        {
            return new PlatformIdentity(steamId, null, $"steam:{steamId}");
        }

        var xuid = ExtractPrefixedId(raw, "Xbox_", XuidPattern);
        if (xuid is not null)
        {
            return new PlatformIdentity(null, xuid, $"xbox:{xuid}");
        }

        var psnId = ExtractPrefixedId(raw, "PlayStation_", PlatformIdSegmentPattern);
        if (psnId is not null)
        {
            return new PlatformIdentity(null, null, $"psn:{psnId}");
        }

        if (raw.StartsWith("Crossplay_", StringComparison.OrdinalIgnoreCase))
        {
            return new PlatformIdentity(null, null, $"crossplay:{NormalizePlatformIdSegment(raw)}");
        }

        return new PlatformIdentity(null, null, $"valheim:{NormalizePlatformIdSegment(raw)}");
    }

    /// <summary>
    /// Valheim's ban list stores a bare SteamID64 for <c>ban &lt;name&gt;</c> on a connected Steam
    /// player, while players are keyed by <c>Steam_&lt;SteamID64&gt;</c>; both name the same account.
    /// </summary>
    public static string ToCanonicalPlatformUserId(string value)
    {
        var raw = (value ?? string.Empty).Trim();
        return BareSteamIdPattern.IsMatch(raw) ? $"Steam_{raw}" : raw;
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
            || Matches(player.XboxLiveId, needle)
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
                || Matches(player.SteamId, needle)
                || Matches(player.XboxLiveId, needle))
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

    private static string? ExtractSteamId(string value)
    {
        if (value.StartsWith("Xbox_", StringComparison.OrdinalIgnoreCase)
            || value.StartsWith("PlayStation_", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        var match = SteamIdPattern.Match(value);
        return match.Success ? match.Groups["steamId"].Value : null;
    }

    private static string? ExtractPrefixedId(string value, string prefix, Regex pattern)
    {
        if (!value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        var id = value.Substring(prefix.Length);
        return pattern.IsMatch(id) ? id : null;
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
