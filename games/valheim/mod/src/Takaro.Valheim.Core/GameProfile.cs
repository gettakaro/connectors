using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Takaro.Valheim.Core;

/// <summary>
/// How this connector presents the game to Takaro: the game and connector identity sent in
/// identify, and the game.profile capability (display name, running game build, icon).
/// </summary>
public sealed class GameProfile
{
    public const string GameIdentifier = "valheim";
    public const string ConnectorName = "takaro-valheim";
    public const string DisplayName = "Valheim";
    public const string IconResourceName = "Takaro.Valheim.icon-256.png";
    public const int MaximumIconBytes = 128 * 1024;
    private const int MaximumGameVersionLength = 32;

    // Semantic Versioning 2.0.0, as Takaro validates connector.version.
    private static readonly Regex StrictSemver = new(
        @"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$",
        RegexOptions.CultureInvariant);

    private readonly byte[]? icon;

    public GameProfile(string connectorVersion, string? gameVersion, byte[]? icon)
    {
        if (connectorVersion.Length > 64 || !StrictSemver.IsMatch(connectorVersion))
        {
            throw new ArgumentException($"Connector version '{connectorVersion}' is not a strict semantic version.", nameof(connectorVersion));
        }

        ConnectorVersion = connectorVersion;
        GameVersion = NormalizeGameVersion(gameVersion);
        if (icon is { Length: > 0 and <= MaximumIconBytes })
        {
            // A copy: the served bytes must always be the ones the revision was computed from.
            this.icon = (byte[])icon.Clone();
            IconRevision = RevisionOf(icon);
        }
    }

    public string ConnectorVersion { get; }

    /// <summary>The running game build, or null when the server did not expose a valid one.</summary>
    public string? GameVersion { get; }

    /// <summary>The first 12 hex characters of the icon's sha256; null without an icon.</summary>
    public string? IconRevision { get; }

    public int IconSize => icon?.Length ?? 0;

    /// <summary>The PNG shipped inside this assembly, or null if it is missing.</summary>
    public static byte[]? LoadEmbeddedIcon()
    {
        using var stream = typeof(GameProfile).Assembly.GetManifestResourceStream(IconResourceName);
        if (stream is null)
        {
            return null;
        }

        using var copy = new MemoryStream();
        stream.CopyTo(copy);
        return copy.ToArray();
    }

    public static string RevisionOf(byte[] bytes)
    {
        using var sha = SHA256.Create();
        return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").Substring(0, 12).ToLowerInvariant();
    }

    /// <summary>Takaro accepts 1 to 32 printable ASCII characters; anything else is omitted, never cut.</summary>
    public static string? NormalizeGameVersion(string? gameVersion)
    {
        var trimmed = gameVersion?.Trim();
        if (string.IsNullOrEmpty(trimmed) || trimmed!.Length > MaximumGameVersionLength)
        {
            return null;
        }

        return trimmed.All(c => c >= 0x20 && c <= 0x7E) ? trimmed : null;
    }

    public JsonObject ToIdentifyConnector() =>
        new() { ["name"] = ConnectorName, ["version"] = ConnectorVersion };

    /// <remarks>
    /// The icon is always declared, also while the world is still loading: a manifest without it
    /// makes Takaro drop the stored icon.
    /// </remarks>
    public JsonObject ToCapability()
    {
        var parameters = new JsonObject { ["name"] = DisplayName };
        if (GameVersion is not null)
        {
            parameters["version"] = GameVersion;
        }

        if (IconRevision is not null)
        {
            parameters["icon"] = new JsonObject { ["revision"] = IconRevision, ["size"] = IconSize };
        }

        return new JsonObject { ["name"] = "game.profile", ["parameters"] = parameters };
    }

    /// <summary>The getGameIcon answer: the whole PNG, base64, under the declared revision.</summary>
    public TakaroActionResult GetGameIcon() =>
        icon is null
            ? TakaroActionResult.Error(ProtocolErrorCodes.GameError, "The connector has no game icon to serve.")
            : TakaroActionResult.Ok(new { revision = IconRevision, data = Convert.ToBase64String(icon) });
}
