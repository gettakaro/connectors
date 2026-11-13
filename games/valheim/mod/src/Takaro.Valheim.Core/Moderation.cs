using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public sealed record ValheimBan(
    string GameId,
    string Name,
    string? SteamId = null,
    string? PlatformId = null);

public static class ModerationFactory
{
    /// <summary>
    /// Builds the listBans payload. Takaro writes each entry's <c>name</c> onto the matched player
    /// and rejects the whole list when one name is missing, so a ban without a real player name is
    /// left out; Valheim still enforces it from bannedlist.txt.
    /// </summary>
    public static object[] CreateBanEntries(IEnumerable<ValheimBan> bans) =>
        bans
            .Where(ban => !string.IsNullOrWhiteSpace(ban.GameId) && !IsPlaceholderName(ban.GameId, ban.Name))
            .Select(ban => new BanEntry(ToBannedPlayer(ban), string.Empty, null))
            .ToArray();

    /// <summary>
    /// Returns the first candidate that is a real player name for <paramref name="banId"/>, or
    /// null when every candidate is blank or one of the player's own ids.
    /// </summary>
    public static string? PickPlayerName(string banId, params string?[] candidates) =>
        candidates
            .Where(candidate => !IsPlaceholderName(banId, candidate))
            .Select(candidate => candidate!.Trim())
            .FirstOrDefault();

    /// <summary>
    /// True when <paramref name="name"/> is blank or is an id of the banned account rather than a
    /// player name. A name ban (no platform account) is named by its own text.
    /// </summary>
    public static bool IsPlaceholderName(string banId, string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return true;
        }

        var gameId = PlayerMapper.ToCanonicalPlatformUserId(banId);
        var identity = PlayerMapper.ToIdentity(gameId);
        if (!identity.IsPlatformAccount)
        {
            return false;
        }

        var needle = name!.Trim();
        var canonicalName = PlayerMapper.ToCanonicalPlatformUserId(needle);
        return Matches(gameId, canonicalName)
            || Matches(banId.Trim(), needle)
            || Matches(identity.SteamId, needle)
            || Matches(identity.XboxLiveId, needle)
            || Matches(identity.PlatformId, needle);
    }

    // Takaro resolves every listBans player through PlayerService.resolveRef, which needs a
    // platform id when the banned player has never joined this server (no POG to fall back on).
    // Name and IP bans get no platform id: resolveRef would create a phantom player for them.
    private static BannedPlayer ToBannedPlayer(ValheimBan ban)
    {
        var gameId = PlayerMapper.ToCanonicalPlatformUserId(ban.GameId);
        var identity = PlayerMapper.ToIdentity(gameId, ban.SteamId);
        if (!identity.IsPlatformAccount)
        {
            return new BannedPlayer(gameId, ban.Name.Trim(), null, null, null);
        }

        return new BannedPlayer(
            gameId,
            ban.Name.Trim(),
            identity.SteamId,
            identity.XboxLiveId,
            identity.PlatformId);
    }

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

    private static bool Matches(string? value, string needle) =>
        value is not null && value.Equals(needle, StringComparison.OrdinalIgnoreCase);

    private sealed record BanEntry(
        [property: JsonPropertyName("player")] BannedPlayer Player,
        [property: JsonPropertyName("reason")] string Reason,
        [property: JsonPropertyName("expiresAt")] DateTimeOffset? ExpiresAt);

    private sealed record BannedPlayer(
        [property: JsonPropertyName("gameId")] string GameId,
        [property: JsonPropertyName("name")] string Name,
        [property: JsonPropertyName("steamId")]
        [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? SteamId,
        [property: JsonPropertyName("xboxLiveId")]
        [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? XboxLiveId,
        [property: JsonPropertyName("platformId")]
        [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? PlatformId);
}
