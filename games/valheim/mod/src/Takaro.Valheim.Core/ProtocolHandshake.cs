using System.Text.Json;
using System.Text.Json.Nodes;

namespace Takaro.Valheim.Core;

public static class ProtocolErrorCodes
{
    public const string Unsupported = "unsupported";
    public const string InvalidArgs = "invalid_args";
    public const string NotFound = "not_found";
    public const string GameError = "game_error";
    public const string Internal = "internal";
    public const string NoSharedProtocolVersion = "no_shared_protocol_version";
}

public static class GameEventTypes
{
    public const string Log = "log";
    public const string PlayerConnected = "player-connected";
    public const string PlayerDisconnected = "player-disconnected";
    public const string ChatMessage = "chat-message";
    public const string PlayerDeath = "player-death";
    public const string EntityKilled = "entity-killed";

    public static IReadOnlyList<string> All { get; } =
        [Log, PlayerConnected, PlayerDisconnected, ChatMessage, PlayerDeath, EntityKilled];
}

/// <summary>Takaro's answer to identify. A rejection carries no game server id.</summary>
public sealed record IdentifyReply(
    bool Accepted,
    string? GameServerId,
    int ProtocolVersion,
    string? ErrorCode,
    string? Reason);

/// <summary>
/// The identify handshake of Generic Game Protocol v1: offered versions, the Capability
/// Manifest, how Takaro's answer is read, and how events are numbered into a stream. Version 0
/// stays fully supported: an older Takaro, or one that picks 0, gets the version 0 wire format.
/// </summary>
public static class ProtocolHandshake
{
    public static IReadOnlyList<int> OfferedProtocolVersions { get; } = [0, 1];

    /// <summary>
    /// The live-supported actions of capabilities.json and their capability names.
    /// getPlayer is unsupported, listLocations and the version 0 map are schema-fallback, so a
    /// version 1 Takaro never asks for them; testReachability is always allowed.
    /// </summary>
    public static IReadOnlyDictionary<string, string> ActionCapabilities { get; } = new Dictionary<string, string>
    {
        [TakaroActionNames.GetPlayers] = "players.list",
        [TakaroActionNames.GetPlayerLocation] = "players.location",
        [TakaroActionNames.GetPlayerInventory] = "players.inventory",
        [TakaroActionNames.TeleportPlayer] = "players.teleport",
        [TakaroActionNames.ListItems] = "items.list",
        [TakaroActionNames.GiveItem] = "items.give",
        [TakaroActionNames.ListEntities] = "entities.list",
        [TakaroActionNames.SendMessage] = "chat.send",
        [TakaroActionNames.ExecuteConsoleCommand] = "console.execute",
        [TakaroActionNames.KickPlayer] = "moderation.kick",
        [TakaroActionNames.BanPlayer] = "moderation.ban",
        [TakaroActionNames.UnbanPlayer] = "moderation.unban",
        [TakaroActionNames.ListBans] = "moderation.listBans",
        [TakaroActionNames.Shutdown] = "server.shutdown",
    };

    public static JsonArray BuildCapabilityManifest(GameProfile? profile = null, bool includeMap = false)
    {
        var manifest = new JsonArray();
        foreach (var capability in ActionCapabilities.Values)
        {
            manifest.Add(new JsonObject { ["name"] = capability });
        }

        foreach (var eventType in GameEventTypes.All)
        {
            manifest.Add(new JsonObject { ["name"] = "events." + eventType });
        }

        if (profile is not null)
        {
            manifest.Add(profile.ToCapability());
        }

        if (includeMap)
        {
            manifest.Add(WorldMapFrame.CreateCapability());
        }

        return manifest;
    }

    /// <summary>Whether a version 1 connection may run the action; Takaro refuses the rest itself.</summary>
    public static bool IsDeclared(string action, GameProfile? profile = null, bool mapDeclared = false) =>
        action == TakaroActionNames.TestReachability
        || ActionCapabilities.ContainsKey(action)
        || (action == TakaroActionNames.GetGameIcon && profile?.IconRevision is not null)
        || (action == TakaroActionNames.GetMapTile && mapDeclared);

    public static IdentifyReply ParseIdentifyResponse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        var payload = root.TryGetProperty("payload", out var payloadElement) ? payloadElement : default;

        // Version 1 rejections put the error next to a null payload and close the socket;
        // version 0 rejections keep it inside the payload.
        if (TryGetError(root, out var error) || (payload.ValueKind == JsonValueKind.Object && TryGetError(payload, out error)))
        {
            var code = error.ValueKind == JsonValueKind.Object
                && error.TryGetProperty("code", out var codeElement)
                && codeElement.ValueKind == JsonValueKind.String
                    ? codeElement.GetString()
                    : null;
            return new IdentifyReply(false, null, 0, code, TakaroProtocol.DescribeError(error));
        }

        if (payload.ValueKind != JsonValueKind.Object)
        {
            return new IdentifyReply(false, null, 0, null, "identifyResponse carries no payload");
        }

        var version = 0;
        if (payload.TryGetProperty("protocolVersion", out var versionElement) && versionElement.ValueKind != JsonValueKind.Null)
        {
            if (versionElement.ValueKind != JsonValueKind.Number
                || !versionElement.TryGetInt32(out version)
                || !OfferedProtocolVersions.Contains(version))
            {
                return new IdentifyReply(
                    false,
                    null,
                    0,
                    ProtocolErrorCodes.NoSharedProtocolVersion,
                    $"Takaro selected protocol version {versionElement.GetRawText()}, which this connector does not speak");
            }
        }

        var gameServerId = payload.TryGetProperty("gameServerId", out var id) && id.ValueKind == JsonValueKind.String
            ? id.GetString()
            : null;
        return gameServerId is null
            ? new IdentifyReply(false, null, version, null, "identifyResponse carries no gameServerId")
            : new IdentifyReply(true, gameServerId, version, null, null);
    }

    /// <summary>Adds the version 1 stream position next to the event's type and data.</summary>
    public static string WithStreamPosition(string eventFrame, string streamId, long seq)
    {
        var frame = JsonNode.Parse(eventFrame)!.AsObject();
        var payload = frame["payload"]!.AsObject();
        payload["streamId"] = streamId;
        payload["seq"] = seq;
        return frame.ToJsonString();
    }

    public static bool TryParseEventAck(string json, out string streamId, out long upTo)
    {
        streamId = string.Empty;
        upTo = 0;
        using var doc = JsonDocument.Parse(json);
        if (!IsType(doc.RootElement, "eventAck")
            || !doc.RootElement.TryGetProperty("payload", out var payload)
            || payload.ValueKind != JsonValueKind.Object
            || !payload.TryGetProperty("streamId", out var stream)
            || stream.ValueKind != JsonValueKind.String
            || !payload.TryGetProperty("upTo", out var upToElement)
            || upToElement.ValueKind != JsonValueKind.Number
            || !upToElement.TryGetInt64(out upTo))
        {
            return false;
        }

        streamId = stream.GetString()!;
        return streamId.Length > 0;
    }

    /// <summary>A one-line description of an eventError frame, or null for any other frame.</summary>
    public static string? DescribeEventError(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        if (!IsType(root, "eventError"))
        {
            return null;
        }

        var seq = root.TryGetProperty("payload", out var payload)
            && payload.ValueKind == JsonValueKind.Object
            && payload.TryGetProperty("seq", out var seqElement)
                ? seqElement.GetRawText()
                : "?";
        var reason = root.TryGetProperty("error", out var error) ? TakaroProtocol.DescribeError(error) : "no reason given";
        return $"seq {seq}: {reason}";
    }

    /// <summary>
    /// Maps the connector's own error codes onto the fixed version 1 set; Takaro turns an
    /// unknown code into internal, which would hide the difference between them.
    /// </summary>
    public static string ToProtocolErrorCode(string? errorCode) => errorCode switch
    {
        null or "" or "action_failed" => ProtocolErrorCodes.Internal,
        ProtocolErrorCodes.InvalidArgs or "invalid_quality" or "invalid_message" => ProtocolErrorCodes.InvalidArgs,
        "player_not_found" or "item_not_found" or "map_not_ready" or "tile_out_of_range" => ProtocolErrorCodes.NotFound,
        "unsupported_action" or CompanionInventoryActionPolicy.UnsupportedErrorCode => ProtocolErrorCodes.Unsupported,
        ProtocolErrorCodes.Unsupported or ProtocolErrorCodes.NotFound or ProtocolErrorCodes.GameError or ProtocolErrorCodes.Internal => errorCode,
        _ => ProtocolErrorCodes.GameError,
    };

    private static bool TryGetError(JsonElement element, out JsonElement error) =>
        element.TryGetProperty("error", out error)
        && error.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined or JsonValueKind.False)
        && !(error.ValueKind == JsonValueKind.String && error.GetString()!.Length == 0);

    private static bool IsType(JsonElement root, string type) =>
        root.ValueKind == JsonValueKind.Object
        && root.TryGetProperty("type", out var typeElement)
        && typeElement.ValueKind == JsonValueKind.String
        && typeElement.GetString() == type;
}
