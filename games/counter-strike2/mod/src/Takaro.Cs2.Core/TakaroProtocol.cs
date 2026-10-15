using System.Text.Json;

namespace Takaro.Cs2.Core;

public sealed record TakaroRequest(string RequestId, string Action, JsonElement Args);

public static class TakaroProtocol
{
    public static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public static string CreateIdentify(ConnectorConfig config)
    {
        return JsonSerializer.Serialize(new
        {
            type = "identify",
            payload = new
            {
                identityToken = NullIfWhiteSpace(config.IdentityToken),
                registrationToken = NullIfWhiteSpace(config.RegistrationToken),
                name = config.ServerName
            }
        }, JsonOptions);
    }

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

        var error = $"{result.Error?.Code ?? "action_failed"}: {result.Error?.Message ?? "CS2 action failed."}";
        return action switch
        {
            "testReachability" => CreateResponse(requestId, new { connectable = false, reason = error, error }),
            "getPlayer" => CreateResponse(requestId, new { gameId = string.Empty, name = string.Empty, error }),
            "getPlayerLocation" => CreateResponse(requestId, new { x = 0d, y = 0d, z = 0d, error }),
            "executeConsoleCommand" => CreateResponse(requestId, new { success = false, rawResult = string.Empty, errorMessage = error, error }),
            _ => CreateResponse(requestId, new { error })
        };
    }

    public static string CreateGameEvent(string eventType, object data)
    {
        return JsonSerializer.Serialize(new
        {
            type = "gameEvent",
            payload = new
            {
                type = eventType,
                data
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

    private static object? NormalizeResponsePayload(object? payload)
    {
        if (payload is not TakaroActionResult actionResult)
        {
            return payload;
        }

        if (actionResult.Success)
        {
            return actionResult.Payload;
        }

        return new { error = $"{actionResult.Error?.Code ?? "action_failed"}: {actionResult.Error?.Message ?? "CS2 action failed."}" };
    }
}
