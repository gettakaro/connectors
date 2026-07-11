using System.Reflection;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionProtocolTests
{
    private const string ProtocolNamespace = "Takaro.Valheim.Companion.Protocol";

    private static readonly JsonSerializerOptions WireJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private static readonly string[] VersionOnePayloadTypeNames =
    [
        "CompanionHello",
        "CompanionHelloAck",
        "CompanionHeartbeat",
        "CompanionChatReport",
        "CompanionInventoryReport",
        "CompanionPlayerDeathReport",
        "CompanionEntityKilledReport"
    ];

    [TestMethod]
    public void VersionOneProtocolConstantsAreStable()
    {
        Assert.AreEqual("TakaroCompanionV1", GetConstant<string>("CompanionProtocol", "RpcName"));
        Assert.AreEqual(1, GetConstant<int>("CompanionProtocol", "CurrentVersion"));
        Assert.AreEqual(1, GetConstant<int>("CompanionProtocol", "MinimumVersion"));
    }

    [TestMethod]
    public void VersionOneCapabilityValuesAreStable()
    {
        var capabilityType = RequireType("CompanionCapability");

        Assert.IsTrue(capabilityType.IsEnum);
        Assert.IsTrue(capabilityType.IsDefined(typeof(FlagsAttribute), inherit: false));
        Assert.AreEqual(0, GetEnumValue(capabilityType, "None"));
        Assert.AreEqual(1, GetEnumValue(capabilityType, "Chat"));
        Assert.AreEqual(2, GetEnumValue(capabilityType, "Inventory"));
        Assert.AreEqual(4, GetEnumValue(capabilityType, "PlayerDeath"));
        Assert.AreEqual(8, GetEnumValue(capabilityType, "EntityKilled"));
    }

    [TestMethod]
    public void VersionOneBoundsAreStableAndFinite()
    {
        Assert.AreEqual(64 * 1024, GetConstant<int>("CompanionProtocol", "MaximumEnvelopeUtf8Bytes"));
        Assert.AreEqual(512, GetConstant<int>("CompanionProtocol", "MaximumChatCharacters"));
        Assert.AreEqual(256, GetConstant<int>("CompanionProtocol", "MaximumInventoryStacks"));
        Assert.AreEqual(128, GetConstant<int>("CompanionProtocol", "MaximumCodeCharacters"));
        Assert.AreEqual(64, GetConstant<int>("CompanionProtocol", "MaximumEventIdCharacters"));
    }

    [TestMethod]
    public void VersionOneMessageTypesAreStable()
    {
        Assert.AreEqual("hello", GetConstant<string>("CompanionMessageTypes", "Hello"));
        Assert.AreEqual("hello-ack", GetConstant<string>("CompanionMessageTypes", "HelloAck"));
        Assert.AreEqual("heartbeat", GetConstant<string>("CompanionMessageTypes", "Heartbeat"));
        Assert.AreEqual("chat", GetConstant<string>("CompanionMessageTypes", "Chat"));
        Assert.AreEqual("inventory-snapshot", GetConstant<string>("CompanionMessageTypes", "InventorySnapshot"));
        Assert.AreEqual("player-death", GetConstant<string>("CompanionMessageTypes", "PlayerDeath"));
        Assert.AreEqual("entity-killed", GetConstant<string>("CompanionMessageTypes", "EntityKilled"));
    }

    [TestMethod]
    public void VersionOneEnvelopeHasOnlyTransportMetadataAndPayload()
    {
        AssertPublicProperties(
            RequireType("CompanionEnvelope"),
            "ProtocolVersion",
            "SessionNonce",
            "Sequence",
            "MessageId",
            "Type",
            "Payload");
    }

    [TestMethod]
    public void VersionOneDefinesRequiredControlAndReportPayloads()
    {
        foreach (var typeName in VersionOnePayloadTypeNames)
        {
            Assert.IsNotNull(RequireType(typeName), typeName);
        }
    }

    [TestMethod]
    public void VersionOneInventoryReportsDescribeOnlyInventoryContent()
    {
        AssertPublicProperties(RequireType("CompanionInventoryReport"), "Stacks");
        AssertPublicProperties(
            RequireType("CompanionInventoryStack"),
            "Code",
            "Name",
            "Amount",
            "Quality",
            "Durability",
            "Equipped",
            "Slot");
    }

    [TestMethod]
    public void VersionOneCombatReportsCarryExplicitBoundedHints()
    {
        AssertPublicProperties(
            RequireType("CompanionPlayerDeathReport"),
            "EventId",
            "TimestampUnixMilliseconds",
            "Position",
            "CauseHint",
            "AttackerCodeHint");
        AssertPublicProperties(
            RequireType("CompanionEntityKilledReport"),
            "EventId",
            "TimestampUnixMilliseconds",
            "Position",
            "EntityCodeHint",
            "WeaponCodeHint");
    }

    [TestMethod]
    public void VersionOneReportsContainNoClaimedPlayerIdentity()
    {
        foreach (var typeName in VersionOnePayloadTypeNames.Append("CompanionEnvelope"))
        {
            foreach (var property in EnumerateProtocolProperties(RequireType(typeName)))
            {
                Assert.IsFalse(
                    IsClaimedPlayerIdentity(property.Name),
                    $"{property.DeclaringType!.Name}.{property.Name} must not claim player identity; the server derives identity from the RPC peer/session.");
            }
        }
    }

    [TestMethod]
    public void RoundTripsEveryVersionOneMessage()
    {
        AssertRoundTrip(
            CompanionMessageTypes.Hello,
            new CompanionHello(1, 1, CompanionCapability.Chat | CompanionCapability.Inventory),
            sequence: 1);
        AssertRoundTrip(
            CompanionMessageTypes.HelloAck,
            new CompanionHelloAck(1, CompanionCapability.Chat),
            sequence: 2);
        AssertRoundTrip(
            CompanionMessageTypes.Heartbeat,
            new CompanionHeartbeat(1_725_000_000_000),
            sequence: 3);
        AssertRoundTrip(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("chat-1", 1_725_000_000_001, "Hello from Valheim"),
            sequence: 4);
        AssertRoundTrip(
            CompanionMessageTypes.InventorySnapshot,
            new CompanionInventoryReport(
            [
                new CompanionInventoryStack("Wood", "Wood", 12, 1, 100.0f, false, 0)
            ]),
            sequence: 5);
        AssertRoundTrip(
            CompanionMessageTypes.PlayerDeath,
            new CompanionPlayerDeathReport(
                "death-1",
                1_725_000_000_002,
                new CompanionPosition(1.25f, 2.5f, -3.75f),
                "fall",
                null),
            sequence: 6);
        AssertRoundTrip(
            CompanionMessageTypes.EntityKilled,
            new CompanionEntityKilledReport(
                "kill-1",
                1_725_000_000_003,
                new CompanionPosition(-4.0f, 5.0f, 6.0f),
                "Greydwarf",
                "Club"),
            sequence: 7);
    }

    [TestMethod]
    public void RejectsEnvelopeOverMaximumUtf8BytesBeforeJsonParsing()
    {
        var oversizedMalformedJson = "{" + new string('\u00e9', CompanionProtocol.MaximumEnvelopeUtf8Bytes);

        Assert.IsTrue(
            Encoding.UTF8.GetByteCount(oversizedMalformedJson) > CompanionProtocol.MaximumEnvelopeUtf8Bytes);
        Assert.IsFalse(
            CompanionEnvelopeCodec.TryDecodeEnvelope(oversizedMalformedJson, out var envelope, out var errorCode));
        Assert.IsNull(envelope);
        Assert.AreEqual("envelope-too-large", errorCode);
    }

    [TestMethod]
    public void RejectsUnknownMessageTypeAndProtocolVersion()
    {
        AssertEnvelopeRejected(
            CreateEnvelopeJson(type: "future-message", payloadJson: "{}"),
            "unknown-message-type");
        AssertEnvelopeRejected(
            CreateEnvelopeJson(protocolVersion: 2, payloadJson: "{}"),
            "unsupported-protocol-version");
        AssertEnvelopeRejected("{not-json", "malformed-json");
        AssertEnvelopeRejected(
            """
            {"protocolVersion":1,"sessionNonce":"nonce","sequence":1,"messageId":"message-1","type":"heartbeat"}
            """,
            "invalid-envelope-fields");
        AssertEnvelopeRejected(
            """
            {"protocolVersion":1,"protocolVersion":1,"sessionNonce":"nonce","sequence":1,"messageId":"message-1","type":"heartbeat","payload":{}}
            """,
            "invalid-envelope-fields");
        AssertEnvelopeRejected(
            """
            {"protocolVersion":1,"sessionNonce":"nonce","sequence":1,"messageId":"message-1","type":"heartbeat","payload":{},"playerId":"claimed"}
            """,
            "invalid-envelope-fields");
        AssertEnvelopeRejected(
            """
            {"ProtocolVersion":1,"sessionNonce":"nonce","sequence":1,"messageId":"message-1","type":"heartbeat","payload":{}}
            """,
            "invalid-envelope-fields");
    }

    [TestMethod]
    public void NegotiatesHighestOverlappingProtocolVersion()
    {
        Assert.IsTrue(CompanionVersionPolicy.TryNegotiate(1, 3, 2, 4, out var selected));
        Assert.AreEqual(3, selected);

        Assert.IsTrue(CompanionVersionPolicy.TryNegotiate(1, 1, 1, 1, out selected));
        Assert.AreEqual(1, selected);
    }

    [TestMethod]
    public void RejectsNonOverlappingProtocolVersionRange()
    {
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(1, 1, 2, 3, out var selected));
        Assert.AreEqual(0, selected);
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(2, 3, 1, 1, out selected));
        Assert.AreEqual(0, selected);
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(0, 1, 1, 1, out selected));
        Assert.AreEqual(0, selected);
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(2, 1, 1, 1, out selected));
        Assert.AreEqual(0, selected);
    }

    [TestMethod]
    public void RejectsMissingNonceMessageIdAndNonPositiveSequence()
    {
        AssertEnvelopeRejected(CreateEnvelopeJson(sessionNonce: ""), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(sessionNonce: "   "), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(sessionNonceJson: "null"), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(messageId: ""), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(messageId: "\t"), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(messageId: new string('m', 65)), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(sequence: 0), "invalid-envelope-metadata");
        AssertEnvelopeRejected(CreateEnvelopeJson(sequence: -1), "invalid-envelope-metadata");
    }

    [TestMethod]
    public void RejectsUnknownPayloadFieldsThatCouldClaimIdentity()
    {
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """
            {"eventId":"chat-1","timestampUnixMilliseconds":1,"message":"hello","playerId":"claimed"}
            """,
            "invalid-payload-fields");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """
            {"eventId":"chat-1","timestampUnixMilliseconds":1,"message":"hello","steamId":"claimed"}
            """,
            "invalid-payload-fields");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """
            {"timestampUnixMilliseconds":1,"message":"hello"}
            """,
            "invalid-payload-fields");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """
            {"eventId":"chat-1","eventId":"chat-2","timestampUnixMilliseconds":1,"message":"hello"}
            """,
            "invalid-payload-fields");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """
            {"EventId":"chat-1","timestampUnixMilliseconds":1,"message":"hello"}
            """,
            "invalid-payload-fields");

        var heartbeatEnvelope = CreateEnvelope(
            CompanionMessageTypes.Heartbeat,
            """{"timestampUnixMilliseconds":1}""");
        Assert.IsFalse(
            CompanionEnvelopeCodec.TryDecodePayload<CompanionChatReport>(
                heartbeatEnvelope,
                out var wrongPayload,
                out var wrongPayloadError));
        Assert.IsNull(wrongPayload);
        Assert.AreEqual("payload-type-mismatch", wrongPayloadError);

        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            """{"eventId":"chat-1","timestampUnixMilliseconds":1,"message":42}""",
            "invalid-payload");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            CreateChatPayload(new string('e', CompanionProtocol.MaximumEventIdCharacters + 1), "hello"),
            "invalid-payload");
        AssertPayloadRejected<CompanionChatReport>(
            CompanionMessageTypes.Chat,
            CreateChatPayload("chat-1", new string('c', CompanionProtocol.MaximumChatCharacters + 1)),
            "invalid-payload");

        var oversizedStacks = string.Join(
            ",",
            Enumerable.Repeat(
                """{"code":"Wood","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0}""",
                CompanionProtocol.MaximumInventoryStacks + 1));
        AssertPayloadRejected<CompanionInventoryReport>(
            CompanionMessageTypes.InventorySnapshot,
            $"{{\"stacks\":[{oversizedStacks}]}}",
            "invalid-payload");
        AssertPayloadRejected<CompanionInventoryReport>(
            CompanionMessageTypes.InventorySnapshot,
            """
            {"stacks":[{"code":"Wood","name":"Wood","amount":0,"quality":1,"durability":1,"equipped":false,"slot":0}]}
            """,
            "invalid-payload");
        AssertPayloadRejected<CompanionInventoryReport>(
            CompanionMessageTypes.InventorySnapshot,
            """
            {"stacks":[{"code":"Wood","name":"Wood","amount":1,"quality":-1,"durability":1,"equipped":false,"slot":0}]}
            """,
            "invalid-payload");
        AssertPayloadRejected<CompanionInventoryReport>(
            CompanionMessageTypes.InventorySnapshot,
            """
            {"stacks":[{"code":"Wood","name":"Wood","amount":1,"quality":1,"durability":-1,"equipped":false,"slot":0}]}
            """,
            "invalid-payload");

        AssertPayloadRejected<CompanionPlayerDeathReport>(
            CompanionMessageTypes.PlayerDeath,
            """
            {"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"x":1e40,"y":0,"z":0},"causeHint":null,"attackerCodeHint":null}
            """,
            "invalid-payload");
        AssertPayloadRejected<CompanionEntityKilledReport>(
            CompanionMessageTypes.EntityKilled,
            $"{{\"eventId\":\"kill-1\",\"timestampUnixMilliseconds\":1,\"position\":{{\"x\":0,\"y\":0,\"z\":0}},\"entityCodeHint\":\"{new string('x', CompanionProtocol.MaximumCodeCharacters + 1)}\",\"weaponCodeHint\":null}}",
            "invalid-payload");
    }

    private static Assembly ProtocolAssembly => Assembly.Load(ProtocolNamespace);

    private static Type RequireType(string typeName)
    {
        var type = ProtocolAssembly.GetType($"{ProtocolNamespace}.{typeName}");
        Assert.IsNotNull(type, $"Expected protocol type {typeName}.");
        return type!;
    }

    private static T GetConstant<T>(string typeName, string fieldName)
    {
        var field = RequireType(typeName).GetField(fieldName, BindingFlags.Public | BindingFlags.Static);
        Assert.IsNotNull(field, $"Expected public constant {typeName}.{fieldName}.");
        Assert.IsTrue(field!.IsLiteral && !field.IsInitOnly, $"{typeName}.{fieldName} must be a constant.");
        return (T)field.GetRawConstantValue()!;
    }

    private static int GetEnumValue(Type enumType, string name)
    {
        Assert.IsTrue(Enum.IsDefined(enumType, name), $"Expected {enumType.Name}.{name}.");
        return Convert.ToInt32(Enum.Parse(enumType, name));
    }

    private static void AssertPublicProperties(Type type, params string[] expectedNames)
    {
        var actualNames = type
            .GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .Select(property => property.Name)
            .ToArray();

        CollectionAssert.AreEquivalent(expectedNames, actualNames, $"Unexpected public shape for {type.Name}.");
    }

    private static IEnumerable<PropertyInfo> EnumerateProtocolProperties(Type rootType)
    {
        var pending = new Queue<Type>();
        var visited = new HashSet<Type>();
        pending.Enqueue(rootType);

        while (pending.Count > 0)
        {
            var type = pending.Dequeue();
            if (!visited.Add(type))
            {
                continue;
            }

            foreach (var property in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                yield return property;

                var nestedType = UnwrapCollection(property.PropertyType);
                if (nestedType.Namespace == ProtocolNamespace && !nestedType.IsEnum)
                {
                    pending.Enqueue(nestedType);
                }
            }
        }
    }

    private static Type UnwrapCollection(Type type)
    {
        if (type.IsArray)
        {
            return type.GetElementType()!;
        }

        return type.IsGenericType && type.GetGenericArguments().Length == 1
            ? type.GetGenericArguments()[0]
            : type;
    }

    private static bool IsClaimedPlayerIdentity(string propertyName)
    {
        var normalized = new string(propertyName.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());

        return normalized.Contains("claimedplayer", StringComparison.Ordinal)
            || normalized is "playerid" or "playername" or "platformid" or "platformuserid"
                or "steamid" or "xboxid" or "networkid" or "peerid" or "gameid"
                or "userid" or "accountid" or "characterid";
    }

    private static void AssertRoundTrip<T>(string messageType, T payload, long sequence)
        where T : class
    {
        var original = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "session-nonce",
            sequence,
            $"message-{sequence}",
            messageType,
            ToJsonElement(payload));

        var json = CompanionEnvelopeCodec.EncodeEnvelope(original);

        using (var document = JsonDocument.Parse(json))
        {
            CollectionAssert.AreEquivalent(
                new[] { "protocolVersion", "sessionNonce", "sequence", "messageId", "type", "payload" },
                document.RootElement.EnumerateObject().Select(property => property.Name).ToArray());
        }

        Assert.IsTrue(
            CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var decodedEnvelope, out var envelopeError),
            envelopeError);
        Assert.IsNotNull(decodedEnvelope);
        Assert.AreEqual(string.Empty, envelopeError);
        Assert.AreEqual(original.ProtocolVersion, decodedEnvelope.ProtocolVersion);
        Assert.AreEqual(original.SessionNonce, decodedEnvelope.SessionNonce);
        Assert.AreEqual(original.Sequence, decodedEnvelope.Sequence);
        Assert.AreEqual(original.MessageId, decodedEnvelope.MessageId);
        Assert.AreEqual(original.Type, decodedEnvelope.Type);

        Assert.IsTrue(
            CompanionEnvelopeCodec.TryDecodePayload<T>(decodedEnvelope, out var decodedPayload, out var payloadError),
            payloadError);
        Assert.IsNotNull(decodedPayload);
        Assert.AreEqual(string.Empty, payloadError);
        Assert.AreEqual(
            JsonSerializer.Serialize(payload, WireJsonOptions),
            JsonSerializer.Serialize(decodedPayload, WireJsonOptions));
    }

    private static void AssertEnvelopeRejected(string json, string expectedErrorCode)
    {
        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out var errorCode));
        Assert.IsNull(envelope);
        Assert.AreEqual(expectedErrorCode, errorCode);
        Assert.IsFalse(errorCode.Contains("claimed", StringComparison.Ordinal));
    }

    private static void AssertPayloadRejected<T>(string messageType, string payloadJson, string expectedErrorCode)
        where T : class
    {
        var envelope = CreateEnvelope(messageType, payloadJson);

        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodePayload<T>(envelope, out var payload, out var errorCode));
        Assert.IsNull(payload);
        Assert.AreEqual(expectedErrorCode, errorCode);
        Assert.IsFalse(errorCode.Contains("claimed", StringComparison.Ordinal));
        Assert.IsFalse(errorCode.Contains("hello", StringComparison.Ordinal));
        Assert.IsFalse(errorCode.Contains("Wood", StringComparison.Ordinal));
    }

    private static CompanionEnvelope CreateEnvelope(string messageType, string payloadJson)
    {
        return new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "nonce",
            1,
            "message-1",
            messageType,
            ToJsonElement(payloadJson));
    }

    private static string CreateEnvelopeJson(
        int protocolVersion = CompanionProtocol.CurrentVersion,
        string sessionNonce = "nonce",
        string? sessionNonceJson = null,
        long sequence = 1,
        string messageId = "message-1",
        string type = CompanionMessageTypes.Heartbeat,
        string payloadJson = "{\"timestampUnixMilliseconds\":1}")
    {
        var encodedNonce = sessionNonceJson ?? JsonSerializer.Serialize(sessionNonce);
        return $"{{\"protocolVersion\":{protocolVersion},\"sessionNonce\":{encodedNonce},\"sequence\":{sequence},\"messageId\":{JsonSerializer.Serialize(messageId)},\"type\":{JsonSerializer.Serialize(type)},\"payload\":{payloadJson}}}";
    }

    private static string CreateChatPayload(string eventId, string message)
    {
        return $"{{\"eventId\":{JsonSerializer.Serialize(eventId)},\"timestampUnixMilliseconds\":1,\"message\":{JsonSerializer.Serialize(message)}}}";
    }

    private static JsonElement ToJsonElement<T>(T value)
    {
        using var document = JsonDocument.Parse(JsonSerializer.Serialize(value, WireJsonOptions));
        return document.RootElement.Clone();
    }

    private static JsonElement ToJsonElement(string json)
    {
        using var document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }
}
