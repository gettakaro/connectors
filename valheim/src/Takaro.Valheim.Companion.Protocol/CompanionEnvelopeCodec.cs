using System.Text;
using System.Text.Json;

namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionEnvelopeCodec
{
    public const int MaximumSessionNonceCharacters = 128;
    public const int MaximumMessageIdCharacters = 64;

    private const float MaximumAbsolutePositionCoordinate = 1_000_000f;
    private const long MaximumTimestampUnixMilliseconds = 253_402_300_799_999L;

    private const string EnvelopeTooLargeError = "envelope-too-large";
    private const string MalformedJsonError = "malformed-json";
    private const string InvalidEnvelopeFieldsError = "invalid-envelope-fields";
    private const string InvalidEnvelopeMetadataError = "invalid-envelope-metadata";
    private const string UnsupportedProtocolVersionError = "unsupported-protocol-version";
    private const string UnknownMessageTypeError = "unknown-message-type";
    private const string PayloadTypeMismatchError = "payload-type-mismatch";
    private const string InvalidPayloadFieldsError = "invalid-payload-fields";
    private const string InvalidPayloadError = "invalid-payload";

    private static readonly JsonSerializerOptions WireJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false
    };

    private static readonly string[] EnvelopeFields =
    [
        "protocolVersion",
        "sessionNonce",
        "sequence",
        "messageId",
        "type",
        "payload"
    ];

    private static readonly string[] HelloFields =
    [
        "minimumVersion",
        "maximumVersion",
        "capabilities"
    ];

    private static readonly string[] HelloAckFields =
    [
        "protocolVersion",
        "productVersion",
        "acceptedCapabilities"
    ];

    private static readonly string[] HelloNackFields =
    [
        "minimumVersion",
        "maximumVersion",
        "productVersion"
    ];

    private static readonly string[] HeartbeatFields = ["timestampUnixMilliseconds"];

    private static readonly string[] ChatFields =
    [
        "eventId",
        "timestampUnixMilliseconds",
        "message"
    ];

    private static readonly string[] ServerChatFields =
    [
        "sender",
        "message"
    ];

    private static readonly string[] InventoryFields = ["stacks"];

    private static readonly string[] InventoryStackFields =
    [
        "code",
        "name",
        "amount",
        "quality",
        "durability",
        "equipped",
        "slot"
    ];

    private static readonly string[] PlayerDeathRequiredFields =
    [
        "eventId",
        "timestampUnixMilliseconds",
        "position"
    ];

    private static readonly string[] PlayerDeathOptionalFields =
    [
        "causeHint",
        "attackerCodeHint"
    ];

    private static readonly string[] EntityKilledRequiredFields =
    [
        "eventId",
        "timestampUnixMilliseconds",
        "position"
    ];

    private static readonly string[] EntityKilledOptionalFields =
    [
        "entityCodeHint",
        "weaponCodeHint"
    ];

    private static readonly string[] PositionFields = ["x", "y", "z"];

    public static string EncodeEnvelope(CompanionEnvelope envelope)
    {
        if (envelope is null)
        {
            throw new ArgumentNullException(nameof(envelope));
        }

        if (!TryValidateEnvelopeMetadata(envelope, out var errorCode)
            || !TryNormalizePayload(envelope, out var normalizedPayload, out errorCode))
        {
            throw new ArgumentException(errorCode, nameof(envelope));
        }

        var normalizedEnvelope = envelope with { Payload = normalizedPayload };
        var json = JsonSerializer.Serialize(normalizedEnvelope, WireJsonOptions);
        if (Encoding.UTF8.GetByteCount(json) > CompanionProtocol.MaximumEnvelopeUtf8Bytes)
        {
            throw new ArgumentException(EnvelopeTooLargeError, nameof(envelope));
        }

        return json;
    }

    public static bool TryDecodeEnvelope(
        string json,
        out CompanionEnvelope? envelope,
        out string errorCode)
    {
        envelope = null;
        errorCode = string.Empty;

        if (json is null)
        {
            errorCode = MalformedJsonError;
            return false;
        }

        if (Encoding.UTF8.GetByteCount(json) > CompanionProtocol.MaximumEnvelopeUtf8Bytes)
        {
            errorCode = EnvelopeTooLargeError;
            return false;
        }

        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;

            if (root.ValueKind != JsonValueKind.Object
                || !HasStrictFields(root, EnvelopeFields, Array.Empty<string>()))
            {
                errorCode = InvalidEnvelopeFieldsError;
                return false;
            }

            if (!TryReadInt32(root, "protocolVersion", out var protocolVersion)
                || !TryReadString(root, "sessionNonce", out var sessionNonce)
                || !TryReadInt64(root, "sequence", out var sequence)
                || !TryReadString(root, "messageId", out var messageId)
                || !TryReadString(root, "type", out var type))
            {
                errorCode = InvalidEnvelopeMetadataError;
                return false;
            }

            var candidate = new CompanionEnvelope(
                protocolVersion,
                sessionNonce!,
                sequence,
                messageId!,
                type!,
                root.GetProperty("payload").Clone());

            if (!TryValidateEnvelopeMetadata(candidate, out errorCode))
            {
                return false;
            }

            envelope = candidate;
            return true;
        }
        catch (JsonException)
        {
            errorCode = MalformedJsonError;
            return false;
        }
        catch (ArgumentException)
        {
            errorCode = MalformedJsonError;
            return false;
        }
    }

    public static bool TryDecodePayload<T>(
        CompanionEnvelope envelope,
        out T? payload,
        out string errorCode)
        where T : class
    {
        payload = null;
        errorCode = string.Empty;

        if (envelope is null || !IsPayloadTypeForMessage(typeof(T), envelope.Type))
        {
            errorCode = PayloadTypeMismatchError;
            return false;
        }

        try
        {
            if (!TryValidatePayloadShape(envelope.Type, envelope.Payload, out errorCode))
            {
                return false;
            }

            payload = JsonSerializer.Deserialize<T>(envelope.Payload.GetRawText(), WireJsonOptions);

            if (payload is null || !IsSemanticallyValidPayload(payload))
            {
                payload = null;
                errorCode = InvalidPayloadError;
                return false;
            }

            return true;
        }
        catch (JsonException)
        {
            payload = null;
            errorCode = InvalidPayloadError;
            return false;
        }
        catch (NotSupportedException)
        {
            payload = null;
            errorCode = InvalidPayloadError;
            return false;
        }
        catch (ObjectDisposedException)
        {
            payload = null;
            errorCode = InvalidPayloadError;
            return false;
        }
        catch (InvalidOperationException)
        {
            payload = null;
            errorCode = InvalidPayloadError;
            return false;
        }
    }

    public static bool TryInspectHelloAck(
        CompanionEnvelope envelope,
        out CompanionHelloAck? helloAck)
    {
        helloAck = null;
        try
        {
            if (envelope is null
                || envelope.Type != CompanionMessageTypes.HelloAck
                || envelope.Payload.ValueKind != JsonValueKind.Object
                || !HasStrictFields(
                    envelope.Payload,
                    HelloAckFields,
                    Array.Empty<string>())
                || !TryReadInt32(
                    envelope.Payload,
                    "protocolVersion",
                    out var protocolVersion)
                || protocolVersion <= 0
                || !TryReadString(
                    envelope.Payload,
                    "productVersion",
                    out var productVersion)
                || !IsRequiredString(
                    productVersion,
                    CompanionProtocol.MaximumProductVersionCharacters)
                || !TryReadInt32(
                    envelope.Payload,
                    "acceptedCapabilities",
                    out var capabilityValue))
            {
                return false;
            }

            var capabilities = (CompanionCapability)capabilityValue;
            if (!HasKnownCapabilities(capabilities))
            {
                return false;
            }

            helloAck = new CompanionHelloAck(
                protocolVersion,
                productVersion!,
                capabilities);
            return true;
        }
        catch (ObjectDisposedException)
        {
            helloAck = null;
            return false;
        }
        catch (InvalidOperationException)
        {
            helloAck = null;
            return false;
        }
    }

    private static bool TryValidateEnvelopeMetadata(CompanionEnvelope envelope, out string errorCode)
    {
        errorCode = string.Empty;

        if (envelope.ProtocolVersion < CompanionProtocol.MinimumVersion
            || envelope.ProtocolVersion > CompanionProtocol.CurrentVersion)
        {
            errorCode = UnsupportedProtocolVersionError;
            return false;
        }

        if (!IsKnownMessageType(envelope.Type))
        {
            errorCode = UnknownMessageTypeError;
            return false;
        }

        if (!IsRequiredString(envelope.SessionNonce, MaximumSessionNonceCharacters)
            || !IsRequiredString(envelope.MessageId, MaximumMessageIdCharacters)
            || envelope.Sequence <= 0)
        {
            errorCode = InvalidEnvelopeMetadataError;
            return false;
        }

        return true;
    }

    private static bool TryNormalizePayload(
        CompanionEnvelope envelope,
        out JsonElement normalizedPayload,
        out string errorCode)
    {
        normalizedPayload = default;

        switch (envelope.Type)
        {
            case CompanionMessageTypes.Hello:
                return TryNormalizePayload<CompanionHello>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.HelloAck:
                return TryNormalizePayload<CompanionHelloAck>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.HelloNack:
                return TryNormalizePayload<CompanionHelloNack>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.Heartbeat:
                return TryNormalizePayload<CompanionHeartbeat>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.Chat:
                return TryNormalizePayload<CompanionChatReport>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.ServerChat:
                return TryNormalizePayload<CompanionServerChatMessage>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.InventorySnapshot:
                return TryNormalizePayload<CompanionInventoryReport>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.PlayerDeath:
                return TryNormalizePayload<CompanionPlayerDeathReport>(envelope, out normalizedPayload, out errorCode);
            case CompanionMessageTypes.EntityKilled:
                return TryNormalizePayload<CompanionEntityKilledReport>(envelope, out normalizedPayload, out errorCode);
            default:
                errorCode = UnknownMessageTypeError;
                return false;
        }
    }

    private static bool TryNormalizePayload<T>(
        CompanionEnvelope envelope,
        out JsonElement normalizedPayload,
        out string errorCode)
        where T : class
    {
        normalizedPayload = default;

        if (!TryDecodePayload<T>(envelope, out var payload, out errorCode))
        {
            return false;
        }

        normalizedPayload = SerializeToElement(payload!);
        return true;
    }

    private static JsonElement SerializeToElement<T>(T value)
    {
        using var document = JsonDocument.Parse(JsonSerializer.Serialize(value, WireJsonOptions));
        return document.RootElement.Clone();
    }

    private static bool TryValidatePayloadShape(
        string messageType,
        JsonElement payload,
        out string errorCode)
    {
        errorCode = string.Empty;

        if (payload.ValueKind != JsonValueKind.Object)
        {
            errorCode = InvalidPayloadError;
            return false;
        }

        bool validFields;
        switch (messageType)
        {
            case CompanionMessageTypes.Hello:
                validFields = HasStrictFields(payload, HelloFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.HelloAck:
                validFields = HasStrictFields(payload, HelloAckFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.HelloNack:
                validFields = HasStrictFields(payload, HelloNackFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.Heartbeat:
                validFields = HasStrictFields(payload, HeartbeatFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.Chat:
                validFields = HasStrictFields(payload, ChatFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.ServerChat:
                validFields = HasStrictFields(payload, ServerChatFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.InventorySnapshot:
                validFields = HasStrictFields(payload, InventoryFields, Array.Empty<string>());
                break;
            case CompanionMessageTypes.PlayerDeath:
                validFields = HasStrictFields(payload, PlayerDeathRequiredFields, PlayerDeathOptionalFields);
                break;
            case CompanionMessageTypes.EntityKilled:
                validFields = HasStrictFields(payload, EntityKilledRequiredFields, EntityKilledOptionalFields);
                break;
            default:
                errorCode = PayloadTypeMismatchError;
                return false;
        }

        if (!validFields)
        {
            errorCode = InvalidPayloadFieldsError;
            return false;
        }

        if (messageType == CompanionMessageTypes.InventorySnapshot)
        {
            var stacks = payload.GetProperty("stacks");
            if (stacks.ValueKind != JsonValueKind.Array)
            {
                errorCode = InvalidPayloadError;
                return false;
            }

            foreach (var stack in stacks.EnumerateArray())
            {
                if (stack.ValueKind != JsonValueKind.Object)
                {
                    errorCode = InvalidPayloadError;
                    return false;
                }

                if (!HasStrictFields(stack, InventoryStackFields, Array.Empty<string>()))
                {
                    errorCode = InvalidPayloadFieldsError;
                    return false;
                }
            }
        }

        if (messageType == CompanionMessageTypes.PlayerDeath
            || messageType == CompanionMessageTypes.EntityKilled)
        {
            var position = payload.GetProperty("position");
            if (position.ValueKind != JsonValueKind.Object)
            {
                errorCode = InvalidPayloadError;
                return false;
            }

            if (!HasStrictFields(position, PositionFields, Array.Empty<string>()))
            {
                errorCode = InvalidPayloadFieldsError;
                return false;
            }
        }

        return true;
    }

    private static bool HasStrictFields(
        JsonElement element,
        IReadOnlyCollection<string> requiredFields,
        IReadOnlyCollection<string> optionalFields)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (var property in element.EnumerateObject())
        {
            if (!seen.Add(property.Name)
                || (!requiredFields.Contains(property.Name) && !optionalFields.Contains(property.Name)))
            {
                return false;
            }
        }

        foreach (var requiredField in requiredFields)
        {
            if (!seen.Contains(requiredField))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsPayloadTypeForMessage(Type payloadType, string messageType)
    {
        return (payloadType == typeof(CompanionHello) && messageType == CompanionMessageTypes.Hello)
            || (payloadType == typeof(CompanionHelloAck) && messageType == CompanionMessageTypes.HelloAck)
            || (payloadType == typeof(CompanionHelloNack) && messageType == CompanionMessageTypes.HelloNack)
            || (payloadType == typeof(CompanionHeartbeat) && messageType == CompanionMessageTypes.Heartbeat)
            || (payloadType == typeof(CompanionChatReport) && messageType == CompanionMessageTypes.Chat)
            || (payloadType == typeof(CompanionServerChatMessage) && messageType == CompanionMessageTypes.ServerChat)
            || (payloadType == typeof(CompanionInventoryReport) && messageType == CompanionMessageTypes.InventorySnapshot)
            || (payloadType == typeof(CompanionPlayerDeathReport) && messageType == CompanionMessageTypes.PlayerDeath)
            || (payloadType == typeof(CompanionEntityKilledReport) && messageType == CompanionMessageTypes.EntityKilled);
    }

    private static bool IsSemanticallyValidPayload(object payload)
    {
        switch (payload)
        {
            case CompanionHello hello:
                return hello.MinimumVersion > 0
                    && hello.MaximumVersion >= hello.MinimumVersion
                    && HasKnownCapabilities(hello.Capabilities);
            case CompanionHelloAck helloAck:
                return helloAck.ProtocolVersion >= CompanionProtocol.MinimumVersion
                    && helloAck.ProtocolVersion <= CompanionProtocol.CurrentVersion
                    && IsRequiredString(
                        helloAck.ProductVersion,
                        CompanionProtocol.MaximumProductVersionCharacters)
                    && HasKnownCapabilities(helloAck.AcceptedCapabilities);
            case CompanionHelloNack helloNack:
                return helloNack.MinimumVersion > 0
                    && helloNack.MaximumVersion >= helloNack.MinimumVersion
                    && IsRequiredString(
                        helloNack.ProductVersion,
                        CompanionProtocol.MaximumProductVersionCharacters);
            case CompanionHeartbeat heartbeat:
                return IsValidTimestamp(heartbeat.TimestampUnixMilliseconds);
            case CompanionChatReport chat:
                return IsEventId(chat.EventId)
                    && IsValidTimestamp(chat.TimestampUnixMilliseconds)
                    && IsRequiredString(chat.Message, CompanionProtocol.MaximumChatCharacters);
            case CompanionServerChatMessage serverChat:
                return IsRequiredString(serverChat.Sender, CompanionProtocol.MaximumChatCharacters)
                    && IsRequiredString(serverChat.Message, CompanionProtocol.MaximumChatCharacters);
            case CompanionInventoryReport inventory:
                return IsValidInventory(inventory);
            case CompanionPlayerDeathReport playerDeath:
                return IsEventId(playerDeath.EventId)
                    && IsValidTimestamp(playerDeath.TimestampUnixMilliseconds)
                    && IsValidPosition(playerDeath.Position)
                    && IsOptionalString(playerDeath.CauseHint, CompanionProtocol.MaximumChatCharacters)
                    && IsOptionalString(playerDeath.AttackerCodeHint, CompanionProtocol.MaximumCodeCharacters);
            case CompanionEntityKilledReport entityKilled:
                return IsEventId(entityKilled.EventId)
                    && IsValidTimestamp(entityKilled.TimestampUnixMilliseconds)
                    && IsValidPosition(entityKilled.Position)
                    && IsOptionalString(entityKilled.EntityCodeHint, CompanionProtocol.MaximumCodeCharacters)
                    && IsOptionalString(entityKilled.WeaponCodeHint, CompanionProtocol.MaximumCodeCharacters);
            default:
                return false;
        }
    }

    private static bool IsValidInventory(CompanionInventoryReport inventory)
    {
        if (inventory.Stacks is null || inventory.Stacks.Count > CompanionProtocol.MaximumInventoryStacks)
        {
            return false;
        }

        foreach (var stack in inventory.Stacks)
        {
            if (stack is null
                || !IsRequiredString(stack.Code, CompanionProtocol.MaximumCodeCharacters)
                || !IsRequiredString(stack.Name, CompanionProtocol.MaximumChatCharacters)
                || stack.Amount <= 0
                || stack.Amount > CompanionProtocol.MaximumInventoryAmount
                || stack.Quality <= 0
                || stack.Quality > CompanionProtocol.MaximumItemQuality
                || !IsFinite(stack.Durability)
                || stack.Durability < 0
                || stack.Durability > CompanionProtocol.MaximumDurability
                || stack.Slot < 0
                || stack.Slot > CompanionProtocol.MaximumInventorySlot)
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsValidPosition(CompanionPosition? position)
    {
        return position is not null
            && IsValidPositionCoordinate(position.X)
            && IsValidPositionCoordinate(position.Y)
            && IsValidPositionCoordinate(position.Z);
    }

    private static bool IsValidPositionCoordinate(float value)
    {
        return IsFinite(value)
            && value >= -MaximumAbsolutePositionCoordinate
            && value <= MaximumAbsolutePositionCoordinate;
    }

    private static bool IsFinite(float value)
    {
        return !float.IsNaN(value) && !float.IsInfinity(value);
    }

    private static bool HasKnownCapabilities(CompanionCapability capabilities)
    {
        const CompanionCapability knownCapabilities = CompanionCapability.Chat
            | CompanionCapability.Inventory
            | CompanionCapability.PlayerDeath
            | CompanionCapability.EntityKilled
            | CompanionCapability.ServerChat;

        return (capabilities & ~knownCapabilities) == 0;
    }

    private static bool IsKnownMessageType(string? messageType)
    {
        return messageType == CompanionMessageTypes.Hello
            || messageType == CompanionMessageTypes.HelloAck
            || messageType == CompanionMessageTypes.HelloNack
            || messageType == CompanionMessageTypes.Heartbeat
            || messageType == CompanionMessageTypes.Chat
            || messageType == CompanionMessageTypes.ServerChat
            || messageType == CompanionMessageTypes.InventorySnapshot
            || messageType == CompanionMessageTypes.PlayerDeath
            || messageType == CompanionMessageTypes.EntityKilled;
    }

    private static bool IsEventId(string? value)
    {
        return IsRequiredString(value, CompanionProtocol.MaximumEventIdCharacters);
    }

    private static bool IsValidTimestamp(long value)
    {
        return value >= 0 && value <= MaximumTimestampUnixMilliseconds;
    }

    private static bool IsRequiredString(string? value, int maximumCharacters)
    {
        return !string.IsNullOrWhiteSpace(value) && value!.Length <= maximumCharacters;
    }

    private static bool IsOptionalString(string? value, int maximumCharacters)
    {
        return value is null || IsRequiredString(value, maximumCharacters);
    }

    private static bool TryReadInt32(JsonElement element, string propertyName, out int value)
    {
        value = default;
        var property = element.GetProperty(propertyName);
        return property.ValueKind == JsonValueKind.Number && property.TryGetInt32(out value);
    }

    private static bool TryReadInt64(JsonElement element, string propertyName, out long value)
    {
        value = default;
        var property = element.GetProperty(propertyName);
        return property.ValueKind == JsonValueKind.Number && property.TryGetInt64(out value);
    }

    private static bool TryReadString(JsonElement element, string propertyName, out string? value)
    {
        var property = element.GetProperty(propertyName);
        if (property.ValueKind != JsonValueKind.String)
        {
            value = null;
            return false;
        }

        value = property.GetString();
        return value is not null;
    }
}
