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

        var error = FormatActionError(result);
        switch (action)
        {
            case "getPlayer":
                response = CreateResponse(requestId, new
                {
                    gameId = string.Empty,
                    name = string.Empty,
                    error
                });
                return true;
            case "getPlayerLocation":
                // Takaro app-connector 0c63cf1c validates IPosition before Generic checks
                // payload.error. Required coordinates make this an immediate actionable
                // rejection without returning a fabricated position to the caller.
                response = CreateResponse(requestId, new
                {
                    x = 0d,
                    y = 0d,
                    z = 0d,
                    error
                });
                return true;
            case "testReachability":
                // This action bypasses Generic.requestFromServer's payload.error check, so
                // its validated DTO must carry an explicit disconnected result and reason.
                response = CreateResponse(requestId, new
                {
                    connectable = false,
                    reason = error,
                    error
                });
                return true;
            case "executeConsoleCommand":
                response = CreateResponse(requestId, new
                {
                    rawResult = string.Empty,
                    success = false,
                    errorMessage = error,
                    error
                });
                return true;
            case "getPlayers":
            case "getPlayerInventory":
            case "listItems":
            case "listEntities":
            case "listLocations":
            case "listBans":
                // These actions are pinned to array DTOs at Takaro 0c63cf1c. JSON arrays
                // cannot carry a top-level error, while [] or an error-bearing item would
                // fabricate game state. Only actual failure paths are suppressed.
                response = null;
                return false;
            default:
                // Takaro does not validate response DTOs for giveItem, sendMessage,
                // teleport/moderation, or shutdown. Generic.requestFromServer rejects the
                // top-level payload.error immediately and preserves the actionable detail.
                response = CreateResponse(requestId, new { error });
                return true;
        }
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

    private static string FormatActionError(TakaroActionResult result) =>
        $"{result.ErrorCode ?? "action_failed"}: {result.Message ?? "Valheim action failed."}";

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
