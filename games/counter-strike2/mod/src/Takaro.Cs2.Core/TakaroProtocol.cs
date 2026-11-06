using System.Text.Json;

namespace Takaro.Cs2.Core;

public sealed record TakaroRequest(string RequestId, string Action, JsonElement Args);

public static class TakaroProtocol
{
    public static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public const string GameId = "counter-strike2";
    public const string ConnectorName = "takaro-counter-strike2";

    public static string CreateIdentify(ConnectorConfig config, IReadOnlyList<object> capabilities)
    {
        return JsonSerializer.Serialize(new
        {
            type = "identify",
            payload = new
            {
                identityToken = NullIfWhiteSpace(config.IdentityToken),
                registrationToken = NullIfWhiteSpace(config.RegistrationToken),
                name = config.ServerName,
                game = GameId,
                connector = new { name = ConnectorName, version = config.ConnectorVersion },
                protocolVersions = new[] { 1 },
                capabilities
            }
        }, JsonOptions);
    }

    public static string CreateCapabilities(string requestId, IReadOnlyList<object> capabilities) =>
        JsonSerializer.Serialize(new { type = "capabilities", requestId, payload = new { capabilities } }, JsonOptions);

    public static TakaroRequest ParseRequest(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;

        if (!root.TryGetProperty("type", out var type) || type.GetString() != "request")
        {
            throw new ArgumentException("Takaro message is not a request.");
        }

        var requestId = RequiredString(root, "requestId");
        var payload = root.GetProperty("payload");
        var action = RequiredString(payload, "action");
        var args = ParseArgs(payload);

        return new TakaroRequest(requestId, action, args);
    }

    public static string CreateResponse(string requestId, object? payload)
    {
        var responsePayload = NormalizeResponsePayload(payload);
        return JsonSerializer.Serialize(new
        {
            type = "response",
            requestId,
            payload = responsePayload
        }, JsonOptions);
    }

    public static string CreateActionResponse(string requestId, string action, TakaroActionResult result)
    {
        if (result.Success) return CreateResponse(requestId, result.Payload);

        var code = result.Error?.Code ?? "action_failed";
        var message = result.Error?.Message ?? "CS2 action failed.";
        return action switch
        {
            "testReachability" => CreateResponse(requestId, new { connectable = false, reason = $"{code}: {message}" }),
            "getPlayerLocation" when code is "player_not_found" or "position_unavailable" => CreateResponse(requestId, null),
            "executeConsoleCommand" => CreateResponse(requestId, new { success = false, rawResult = string.Empty, errorMessage = $"{code}: {message}" }),
            _ => JsonSerializer.Serialize(new { type = "response", requestId, error = new { code = ToProtocolErrorCode(code), message = $"{code}: {message}" } }, JsonOptions)
        };
    }

    public static string ToProtocolErrorCode(string code) => code switch
    {
        "invalid_args" or "invalid_amount" or "invalid_expiry" or "invalid_player" or "unknown_item" => "invalid_args",
        "unsupported_action" or "shutdown_disabled" => "unsupported",
        "player_not_found" or "ban_not_found" or "no_recipients" or "tile_out_of_range"
            or "map_unavailable" or "locations_unavailable" => "not_found",
        "action_failed" => "internal",
        _ => "game_error"
    };

    public static string CreateGameEvent(string eventType, object data, string streamId, long seq)
    {
        return JsonSerializer.Serialize(new
        {
            type = "gameEvent",
            payload = new
            {
                type = eventType,
                data,
                streamId,
                seq
            }
        }, JsonOptions);
    }

    private static JsonElement ParseArgs(JsonElement payload)
    {
        if (!payload.TryGetProperty("args", out var args))
        {
            using var empty = JsonDocument.Parse("{}");
            return empty.RootElement.Clone();
        }

        if (args.ValueKind == JsonValueKind.String)
        {
            var raw = args.GetString();
            using var parsed = JsonDocument.Parse(string.IsNullOrWhiteSpace(raw) ? "{}" : raw);
            return parsed.RootElement.Clone();
        }

        return args.Clone();
    }

    private static string RequiredString(JsonElement element, string property)
    {
        if (!element.TryGetProperty(property, out var value) || value.ValueKind != JsonValueKind.String)
        {
            throw new ArgumentException($"Takaro message missing string property: {property}");
        }

        var text = value.GetString();
        if (string.IsNullOrWhiteSpace(text))
        {
            throw new ArgumentException($"Takaro message has empty string property: {property}");
        }

        return text;
    }

    private static string? NullIfWhiteSpace(string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : value;

    private static object? NormalizeResponsePayload(object? payload) =>
        payload is TakaroActionResult { Success: true } actionResult ? actionResult.Payload : payload;
}
