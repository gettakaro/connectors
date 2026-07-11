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
    private const int MaximumInventoryAmount = 1_000_000;
    private const int MaximumItemQuality = 1_000_000;
    private const float MaximumDurability = 1_000_000_000f;
    private const float MaximumAbsolutePositionCoordinate = 1_000_000f;
    private const long MaximumTimestampUnixMilliseconds = 253_402_300_799_999L;

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
    public void HelloAckWireShapeRequiresBoundedProductVersion()
    {
        AssertPublicProperties(
            RequireType("CompanionHelloAck"),
            "ProtocolVersion",
            "ProductVersion",
            "AcceptedCapabilities");
        Assert.AreEqual(128, GetConstant<int>("CompanionProtocol", "MaximumProductVersionCharacters"));

        AssertDeclaredPayloadAccepted(
            CompanionMessageTypes.HelloAck,
            $"{{\"protocolVersion\":1,\"productVersion\":\"{new string('v', 128)}\",\"acceptedCapabilities\":1}}");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.HelloAck,
            """{"protocolVersion":1,"acceptedCapabilities":1}""",
            "invalid-payload-fields",
            "hello-ack missing product version");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.HelloAck,
            """{"protocolVersion":1,"ProductVersion":"1.2.3","acceptedCapabilities":1}""",
            "invalid-payload-fields",
            "hello-ack product version casing");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.HelloAck,
            """{"protocolVersion":1,"productVersion":"","acceptedCapabilities":1}""",
            "invalid-payload",
            "hello-ack blank product version");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.HelloAck,
            """{"protocolVersion":1,"productVersion":null,"acceptedCapabilities":1}""",
            "invalid-payload",
            "hello-ack null product version");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.HelloAck,
            $"{{\"protocolVersion\":1,\"productVersion\":\"{new string('v', 129)}\",\"acceptedCapabilities\":1}}",
            "invalid-payload",
            "hello-ack oversized product version");
    }

    [TestMethod]
    public void InspectsUnsupportedHelloAckVersionWithoutAcceptingIt()
    {
        var envelope = CreateEnvelope(
            CompanionMessageTypes.HelloAck,
            """{"protocolVersion":2,"productVersion":"2.4.0","acceptedCapabilities":1}""");

        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(
            envelope,
            out _,
            out var strictError));
        Assert.AreEqual("invalid-payload", strictError);
        Assert.IsTrue(CompanionEnvelopeCodec.TryInspectHelloAck(
            envelope,
            out var inspected));
        Assert.IsNotNull(inspected);
        Assert.AreEqual(2, inspected.ProtocolVersion);
        Assert.AreEqual("2.4.0", inspected.ProductVersion);
        Assert.AreEqual(CompanionCapability.Chat, inspected.AcceptedCapabilities);

        Assert.IsFalse(CompanionEnvelopeCodec.TryInspectHelloAck(
            CreateEnvelope(
                CompanionMessageTypes.HelloAck,
                """{"protocolVersion":2,"productVersion":"2.4.0","acceptedCapabilities":1,"playerId":"claimed"}"""),
            out _));

        CompanionEnvelope disposedEnvelope;
        using (var document = JsonDocument.Parse(
                   """{"protocolVersion":2,"productVersion":"2.4.0","acceptedCapabilities":1}"""))
        {
            disposedEnvelope = new CompanionEnvelope(
                CompanionProtocol.CurrentVersion,
                "nonce",
                1,
                "message-1",
                CompanionMessageTypes.HelloAck,
                document.RootElement);
        }

        Assert.IsFalse(CompanionEnvelopeCodec.TryInspectHelloAck(
            disposedEnvelope,
            out _));
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
            new CompanionHelloAck(1, "1.2.3", CompanionCapability.Chat),
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
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.Chat,
            """
            {"eventId":"chat-1","timestampUnixMilliseconds":1,"message":"hello","playerId":"claimed"}
            """,
            "invalid-payload-fields");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.Chat,
            """
            {"eventId":"chat-1","timestampUnixMilliseconds":1,"message":"hello","steamId":"claimed"}
            """,
            "invalid-payload-fields");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.InventorySnapshot,
            """
            {"stacks":[{"code":"Wood","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0,"accountId":"claimed"}]}
            """,
            "invalid-payload-fields");
        AssertDeclaredPayloadRejected(
            CompanionMessageTypes.PlayerDeath,
            """
            {"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":0,"z":0,"peerId":"claimed"}}
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
    }

    [TestMethod]
    public void RejectsInvalidValuesAcrossEveryVersionOnePayload()
    {
        var oversizedStacks = string.Join(
            ",",
            Enumerable.Repeat(
                """{"code":"Wood","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0}""",
                CompanionProtocol.MaximumInventoryStacks + 1));

        var invalidPayloads = new (string Name, string MessageType, string Json, string ErrorCode)[]
        {
            ("hello nonpositive range", CompanionMessageTypes.Hello,
                """{"minimumVersion":0,"maximumVersion":1,"capabilities":0}""", "invalid-payload"),
            ("hello reversed range", CompanionMessageTypes.Hello,
                """{"minimumVersion":2,"maximumVersion":1,"capabilities":0}""", "invalid-payload"),
            ("hello unknown capability", CompanionMessageTypes.Hello,
                """{"minimumVersion":1,"maximumVersion":1,"capabilities":16}""", "invalid-payload"),
            ("hello-ack unsupported version", CompanionMessageTypes.HelloAck,
                """{"protocolVersion":2,"productVersion":"1.2.3","acceptedCapabilities":0}""", "invalid-payload"),
            ("hello-ack unknown capability", CompanionMessageTypes.HelloAck,
                """{"protocolVersion":1,"productVersion":"1.2.3","acceptedCapabilities":16}""", "invalid-payload"),
            ("heartbeat negative timestamp", CompanionMessageTypes.Heartbeat,
                """{"timestampUnixMilliseconds":-1}""", "invalid-payload"),
            ("chat null event", CompanionMessageTypes.Chat,
                """{"eventId":null,"timestampUnixMilliseconds":1,"message":"hello"}""", "invalid-payload"),
            ("chat negative timestamp", CompanionMessageTypes.Chat,
                """{"eventId":"chat-1","timestampUnixMilliseconds":-1,"message":"hello"}""", "invalid-payload"),
            ("chat null message", CompanionMessageTypes.Chat,
                """{"eventId":"chat-1","timestampUnixMilliseconds":1,"message":null}""", "invalid-payload"),
            ("chat oversized event", CompanionMessageTypes.Chat,
                CreateChatPayload(new string('e', CompanionProtocol.MaximumEventIdCharacters + 1), "hello"), "invalid-payload"),
            ("chat oversized message", CompanionMessageTypes.Chat,
                CreateChatPayload("chat-1", new string('c', CompanionProtocol.MaximumChatCharacters + 1)), "invalid-payload"),
            ("inventory null stacks", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":null}""", "invalid-payload"),
            ("inventory too many stacks", CompanionMessageTypes.InventorySnapshot,
                $"{{\"stacks\":[{oversizedStacks}]}}", "invalid-payload"),
            ("inventory null stack", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[null]}""", "invalid-payload"),
            ("inventory nested unknown", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[{"code":"Wood","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0,"future":true}]}""", "invalid-payload-fields"),
            ("inventory nested duplicate", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[{"code":"Wood","code":"Stone","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0}]}""", "invalid-payload-fields"),
            ("inventory nested casing", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[{"Code":"Wood","name":"Wood","amount":1,"quality":1,"durability":1,"equipped":false,"slot":0}]}""", "invalid-payload-fields"),
            ("inventory null strings", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[{"code":null,"name":null,"amount":1,"quality":1,"durability":1,"equipped":false,"slot":0}]}""", "invalid-payload"),
            ("inventory oversized strings", CompanionMessageTypes.InventorySnapshot,
                $"{{\"stacks\":[{{\"code\":\"{new string('c', CompanionProtocol.MaximumCodeCharacters + 1)}\",\"name\":\"{new string('n', CompanionProtocol.MaximumChatCharacters + 1)}\",\"amount\":1,\"quality\":1,\"durability\":1,\"equipped\":false,\"slot\":0}}]}}", "invalid-payload"),
            ("inventory invalid numbers", CompanionMessageTypes.InventorySnapshot,
                """{"stacks":[{"code":"Wood","name":"Wood","amount":0,"quality":0,"durability":-1,"equipped":false,"slot":-1}]}""", "invalid-payload"),
            ("death null position", CompanionMessageTypes.PlayerDeath,
                """{"eventId":"death-1","timestampUnixMilliseconds":1,"position":null}""", "invalid-payload"),
            ("death nested unknown", CompanionMessageTypes.PlayerDeath,
                """{"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":0,"z":0,"future":1}}""", "invalid-payload-fields"),
            ("death nested duplicate", CompanionMessageTypes.PlayerDeath,
                """{"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"x":0,"x":1,"y":0,"z":0}}""", "invalid-payload-fields"),
            ("death nested casing", CompanionMessageTypes.PlayerDeath,
                """{"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"X":0,"y":0,"z":0}}""", "invalid-payload-fields"),
            ("death nonfinite position", CompanionMessageTypes.PlayerDeath,
                """{"eventId":"death-1","timestampUnixMilliseconds":1,"position":{"x":"NaN","y":0,"z":0}}""", "invalid-payload"),
            ("kill null position", CompanionMessageTypes.EntityKilled,
                """{"eventId":"kill-1","timestampUnixMilliseconds":1,"position":null}""", "invalid-payload"),
            ("kill nested unknown", CompanionMessageTypes.EntityKilled,
                """{"eventId":"kill-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":0,"z":0,"future":1}}""", "invalid-payload-fields"),
            ("kill nested duplicate", CompanionMessageTypes.EntityKilled,
                """{"eventId":"kill-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":0,"z":0,"z":1}}""", "invalid-payload-fields"),
            ("kill nested casing", CompanionMessageTypes.EntityKilled,
                """{"eventId":"kill-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":0,"Z":0}}""", "invalid-payload-fields"),
            ("kill nonfinite position", CompanionMessageTypes.EntityKilled,
                """{"eventId":"kill-1","timestampUnixMilliseconds":1,"position":{"x":0,"y":"Infinity","z":0}}""", "invalid-payload"),
            ("kill oversized code hint", CompanionMessageTypes.EntityKilled,
                $"{{\"eventId\":\"kill-1\",\"timestampUnixMilliseconds\":1,\"position\":{{\"x\":0,\"y\":0,\"z\":0}},\"entityCodeHint\":\"{new string('x', CompanionProtocol.MaximumCodeCharacters + 1)}\"}}", "invalid-payload")
        };

        foreach (var testCase in invalidPayloads)
        {
            AssertDeclaredPayloadRejected(testCase.MessageType, testCase.Json, testCase.ErrorCode, testCase.Name);
        }
    }

    [TestMethod]
    public void RejectsFinitePayloadValuesBeyondWireSafetyBoundsOnDecodeAndEncode()
    {
        var invalidPayloads = new (string Name, string MessageType, object Payload)[]
        {
            ("heartbeat timestamp", CompanionMessageTypes.Heartbeat,
                new CompanionHeartbeat(MaximumTimestampUnixMilliseconds + 1)),
            ("chat timestamp", CompanionMessageTypes.Chat,
                new CompanionChatReport("chat-1", MaximumTimestampUnixMilliseconds + 1, "hello")),
            ("inventory amount", CompanionMessageTypes.InventorySnapshot,
                InventoryWith(new CompanionInventoryStack("Wood", "Wood", MaximumInventoryAmount + 1, 1, 1, false, 0))),
            ("inventory quality", CompanionMessageTypes.InventorySnapshot,
                InventoryWith(new CompanionInventoryStack("Wood", "Wood", 1, MaximumItemQuality + 1, 1, false, 0))),
            ("inventory slot", CompanionMessageTypes.InventorySnapshot,
                InventoryWith(new CompanionInventoryStack("Wood", "Wood", 1, 1, 1, false, CompanionProtocol.MaximumInventoryStacks))),
            ("inventory durability", CompanionMessageTypes.InventorySnapshot,
                InventoryWith(new CompanionInventoryStack("Wood", "Wood", 1, 1, MaximumDurability + 128f, false, 0))),
            ("death timestamp", CompanionMessageTypes.PlayerDeath,
                new CompanionPlayerDeathReport("death-1", MaximumTimestampUnixMilliseconds + 1, new CompanionPosition(0, 0, 0), null, null)),
            ("death coordinate", CompanionMessageTypes.PlayerDeath,
                new CompanionPlayerDeathReport("death-1", 1, new CompanionPosition(MaximumAbsolutePositionCoordinate + 1, 0, 0), null, null)),
            ("kill timestamp", CompanionMessageTypes.EntityKilled,
                new CompanionEntityKilledReport("kill-1", MaximumTimestampUnixMilliseconds + 1, new CompanionPosition(0, 0, 0), null, null)),
            ("kill coordinate", CompanionMessageTypes.EntityKilled,
                new CompanionEntityKilledReport("kill-1", 1, new CompanionPosition(0, 0, -MaximumAbsolutePositionCoordinate - 1), null, null))
        };

        foreach (var testCase in invalidPayloads)
        {
            AssertDeclaredPayloadRejected(
                testCase.MessageType,
                SerializePayload(testCase.Payload),
                "invalid-payload",
                testCase.Name);
        }
    }

    [TestMethod]
    public void AcceptsExactWireSafetyBoundsOnDecodeAndEncode()
    {
        var validPayloads = new (string MessageType, object Payload)[]
        {
            (CompanionMessageTypes.Heartbeat,
                new CompanionHeartbeat(MaximumTimestampUnixMilliseconds)),
            (CompanionMessageTypes.Chat,
                new CompanionChatReport(
                    new string('e', CompanionProtocol.MaximumEventIdCharacters),
                    MaximumTimestampUnixMilliseconds,
                    new string('c', CompanionProtocol.MaximumChatCharacters))),
            (CompanionMessageTypes.InventorySnapshot,
                InventoryWith(new CompanionInventoryStack(
                    new string('c', CompanionProtocol.MaximumCodeCharacters),
                    new string('n', CompanionProtocol.MaximumChatCharacters),
                    MaximumInventoryAmount,
                    MaximumItemQuality,
                    MaximumDurability,
                    true,
                    CompanionProtocol.MaximumInventoryStacks - 1))),
            (CompanionMessageTypes.PlayerDeath,
                new CompanionPlayerDeathReport(
                    "death-1",
                    MaximumTimestampUnixMilliseconds,
                    new CompanionPosition(
                        MaximumAbsolutePositionCoordinate,
                        -MaximumAbsolutePositionCoordinate,
                        MaximumAbsolutePositionCoordinate),
                    new string('c', CompanionProtocol.MaximumChatCharacters),
                    new string('a', CompanionProtocol.MaximumCodeCharacters))),
            (CompanionMessageTypes.EntityKilled,
                new CompanionEntityKilledReport(
                    "kill-1",
                    MaximumTimestampUnixMilliseconds,
                    new CompanionPosition(
                        -MaximumAbsolutePositionCoordinate,
                        MaximumAbsolutePositionCoordinate,
                        -MaximumAbsolutePositionCoordinate),
                    new string('e', CompanionProtocol.MaximumCodeCharacters),
                    new string('w', CompanionProtocol.MaximumCodeCharacters)))
        };

        foreach (var testCase in validPayloads)
        {
            AssertDeclaredPayloadAccepted(testCase.MessageType, SerializePayload(testCase.Payload));
        }
    }

    [TestMethod]
    public void ReturnsStableErrorForDisposedExternalPayloadElement()
    {
        JsonElement disposedPayload;
        using (var document = JsonDocument.Parse("""{"timestampUnixMilliseconds":1}"""))
        {
            disposedPayload = document.RootElement;
        }

        var envelope = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "nonce",
            1,
            "message-1",
            CompanionMessageTypes.Heartbeat,
            disposedPayload);

        Assert.IsFalse(
            CompanionEnvelopeCodec.TryDecodePayload<CompanionHeartbeat>(
                envelope,
                out var payload,
                out var errorCode));
        Assert.IsNull(payload);
        Assert.AreEqual("invalid-payload", errorCode);
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

    private static void AssertPayloadRejected<T>(
        string messageType,
        string payloadJson,
        string expectedErrorCode,
        string caseName)
        where T : class
    {
        var envelope = CreateEnvelope(messageType, payloadJson);

        Assert.IsFalse(
            CompanionEnvelopeCodec.TryDecodePayload<T>(envelope, out var payload, out var errorCode),
            caseName);
        Assert.IsNull(payload, caseName);
        Assert.AreEqual(expectedErrorCode, errorCode, caseName);
        Assert.IsFalse(errorCode.Contains("claimed", StringComparison.Ordinal), caseName);
        Assert.IsFalse(errorCode.Contains("hello", StringComparison.Ordinal), caseName);
        Assert.IsFalse(errorCode.Contains("Wood", StringComparison.Ordinal), caseName);
    }

    private static void AssertDeclaredPayloadRejected(
        string messageType,
        string payloadJson,
        string expectedErrorCode,
        string? caseName = null)
    {
        var name = caseName ?? messageType;

        switch (messageType)
        {
            case CompanionMessageTypes.Hello:
                AssertPayloadRejected<CompanionHello>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.HelloAck:
                AssertPayloadRejected<CompanionHelloAck>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.Heartbeat:
                AssertPayloadRejected<CompanionHeartbeat>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.Chat:
                AssertPayloadRejected<CompanionChatReport>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.InventorySnapshot:
                AssertPayloadRejected<CompanionInventoryReport>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.PlayerDeath:
                AssertPayloadRejected<CompanionPlayerDeathReport>(messageType, payloadJson, expectedErrorCode, name);
                break;
            case CompanionMessageTypes.EntityKilled:
                AssertPayloadRejected<CompanionEntityKilledReport>(messageType, payloadJson, expectedErrorCode, name);
                break;
            default:
                Assert.Fail($"Unknown test message type {messageType}.");
                break;
        }

        var envelope = CreateEnvelope(messageType, payloadJson);
        var exception = Assert.ThrowsException<ArgumentException>(
            () => CompanionEnvelopeCodec.EncodeEnvelope(envelope),
            name);
        StringAssert.StartsWith(exception.Message, expectedErrorCode, name);
        Assert.IsFalse(exception.Message.Contains("claimed", StringComparison.Ordinal), name);
        Assert.IsFalse(exception.Message.Contains("Wood", StringComparison.Ordinal), name);
    }

    private static void AssertDeclaredPayloadAccepted(string messageType, string payloadJson)
    {
        var envelope = CreateEnvelope(messageType, payloadJson);

        switch (messageType)
        {
            case CompanionMessageTypes.HelloAck:
                AssertPayloadAccepted<CompanionHelloAck>(envelope);
                break;
            case CompanionMessageTypes.Heartbeat:
                AssertPayloadAccepted<CompanionHeartbeat>(envelope);
                break;
            case CompanionMessageTypes.Chat:
                AssertPayloadAccepted<CompanionChatReport>(envelope);
                break;
            case CompanionMessageTypes.InventorySnapshot:
                AssertPayloadAccepted<CompanionInventoryReport>(envelope);
                break;
            case CompanionMessageTypes.PlayerDeath:
                AssertPayloadAccepted<CompanionPlayerDeathReport>(envelope);
                break;
            case CompanionMessageTypes.EntityKilled:
                AssertPayloadAccepted<CompanionEntityKilledReport>(envelope);
                break;
            default:
                Assert.Fail($"Unsupported acceptance-test message type {messageType}.");
                break;
        }

        var encoded = CompanionEnvelopeCodec.EncodeEnvelope(envelope);
        Assert.IsTrue(
            CompanionEnvelopeCodec.TryDecodeEnvelope(encoded, out var decodedEnvelope, out var errorCode),
            errorCode);
        Assert.IsNotNull(decodedEnvelope);
    }

    private static void AssertPayloadAccepted<T>(CompanionEnvelope envelope)
        where T : class
    {
        Assert.IsTrue(
            CompanionEnvelopeCodec.TryDecodePayload<T>(envelope, out var payload, out var errorCode),
            errorCode);
        Assert.IsNotNull(payload);
        Assert.AreEqual(string.Empty, errorCode);
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

    private static CompanionInventoryReport InventoryWith(CompanionInventoryStack stack)
    {
        return new CompanionInventoryReport([stack]);
    }

    private static string SerializePayload(object payload)
    {
        return JsonSerializer.Serialize(payload, payload.GetType(), WireJsonOptions);
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
