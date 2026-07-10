using System.Text.Json;

namespace Takaro.Valheim.Core;

public sealed record TakaroRequest(string RequestId, string Action, JsonElement Args);

public static class TakaroProtocol
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public static string CreateIdentify(ConnectorConfig config)
    {
        return JsonSerializer.Serialize(new
        {
            type = "identify",
            payload = new
            {
                identityToken = config.IdentityToken,
                registrationToken = config.RegistrationToken,
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
            throw new ArgumentException("Takaro message is not a request");
        }

        var requestId = RequiredString(root, "requestId");
        var payload = root.GetProperty("payload");
        var action = RequiredString(payload, "action");
        var args = ParseArgs(payload);

        return new TakaroRequest(requestId, action, args);
    }

    public static string CreateResponse(string requestId, object payload)
    {
        var responsePayload = NormalizeResponsePayload(payload);
        return JsonSerializer.Serialize(new
        {
            type = "response",
            requestId,
            payload = responsePayload
        }, JsonOptions);
    }

    public static bool TryCreateActionResponse(
        string requestId,
        string action,
        TakaroActionResult result,
        out string? response)
    {
        if (result.Success)
        {
            response = CreateResponse(requestId, result.Payload!);
            return true;
        }

        if (action.Equals("getPlayerLocation", StringComparison.Ordinal))
        {
            // Current Takaro app-connector 0c63cf1c validates the action DTO, resolves only
            // payload, and Generic.requestFromServer rejects payload.error. Required numeric
            // coordinates keep this compatible with the current non-null IPosition route.
            response = CreateResponse(requestId, new
            {
                x = 0d,
                y = 0d,
                z = 0d,
                error = $"{result.ErrorCode ?? "action_failed"}: {result.Message ?? "Valheim action failed."}"
            });
            return true;
        }

        // Takaro currently has no general action-failure response envelope. In particular,
        // inventory must be an array, and [] would be persisted as a real empty inventory.
        // Sending no frame lets Takaro reject via its bounded pending-request timeout.
        response = null;
        return false;
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
            var raw = args.GetString() ?? "{}";
            using var parsed = JsonDocument.Parse(string.IsNullOrWhiteSpace(raw) ? "{}" : raw);
            return parsed.RootElement.Clone();
        }

        return args.Clone();
    }

    private static string RequiredString(JsonElement element, string property)
    {
        if (!element.TryGetProperty(property, out var value) || value.ValueKind != JsonValueKind.String)
        {
            throw new ArgumentException($"Takaro request missing string property: {property}");
        }

        var text = value.GetString();
        if (string.IsNullOrWhiteSpace(text))
        {
            throw new ArgumentException($"Takaro request has empty string property: {property}");
        }

        return text!;
    }

    private static object NormalizeResponsePayload(object payload)
    {
        if (payload is TakaroActionResult actionResult)
        {
            if (actionResult.Success)
            {
                return actionResult.Payload!;
            }

            return new
            {
                success = false,
                errorCode = actionResult.ErrorCode,
                message = actionResult.Message,
                payload = actionResult.Payload
            };
        }

        return payload;
    }

}

public sealed class SuppressedResponseLogLimiter
{
    private readonly TimeSpan interval;
    private readonly Dictionary<string, DateTimeOffset> lastLoggedAt = new(StringComparer.Ordinal);

    public SuppressedResponseLogLimiter(TimeSpan interval)
    {
        if (interval <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(interval), "Log interval must be positive.");
        }

        this.interval = interval;
    }

    public bool ShouldLog(string action, string? errorCode, DateTimeOffset now)
    {
        var key = $"{action}\n{errorCode ?? "action_failed"}";
        lock (lastLoggedAt)
        {
            if (lastLoggedAt.TryGetValue(key, out var previous) && now - previous < interval)
            {
                return false;
            }

            lastLoggedAt[key] = now;
            return true;
        }
    }
}
