using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionReportProcessorTests
{
    private const long PeerId = 42;
    private const string SessionNonce = "server-session";
    private static readonly DateTimeOffset Now = DateTimeOffset.Parse("2026-07-11T12:00:00+00:00");
    private static readonly TakaroPlayer AuthoritativePlayer = new(
        "Steam_real",
        "Odin",
        "real",
        "steam:real",
        "127.0.0.1",
        12);

    [TestMethod]
    public void ChatUsesAuthoritativeRpcSenderPlayer()
    {
        var harness = CreateHarness(CompanionCapability.Chat);
        var envelope = Envelope(
            CompanionMessageTypes.Chat,
            sequence: 2,
            new CompanionChatReport("chat-1", ToUnixMilliseconds("2026-07-11T11:59:58+00:00"), "$help"));

        var output = harness.Processor.Process(PeerId, AuthoritativePlayer, envelope, Now);

        Assert.IsInstanceOfType<CompanionAcceptedEvent>(output);
        var accepted = (CompanionAcceptedEvent)output!;
        Assert.AreEqual(ValheimEventType.ChatMessage, accepted.Type);
        using var document = Serialize(accepted);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");
        Assert.AreEqual("Steam_real", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Odin", data.GetProperty("player").GetProperty("name").GetString());
        Assert.AreEqual("global", data.GetProperty("channel").GetString());
        Assert.AreEqual("$help", data.GetProperty("msg").GetString());
        Assert.AreEqual("2026-07-11T11:59:58+00:00", data.GetProperty("timestamp").GetString());
    }

    [TestMethod]
    public void InventoryUpdatesCacheWithoutEmittingAnEvent()
    {
        var harness = CreateHarness(CompanionCapability.Inventory);
        var envelope = Envelope(
            CompanionMessageTypes.InventorySnapshot,
            sequence: 2,
            new CompanionInventoryReport(new[]
            {
                new CompanionInventoryStack("SwordIron", "Iron sword", 1, 2, 93.5f, true, 4)
            }));

        var output = harness.Processor.Process(PeerId, AuthoritativePlayer, envelope, Now);

        Assert.IsInstanceOfType<CompanionInventoryUpdated>(output);
        var update = (CompanionInventoryUpdated)output!;
        Assert.AreEqual(AuthoritativePlayer, update.Player);
        Assert.IsFalse(output is CompanionAcceptedEvent);
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            harness.Inventory.TryGet(AuthoritativePlayer.GameId, Now, out var inventory));
        Assert.AreEqual(1, inventory.Count);
        Assert.AreEqual("SwordIron", inventory.Single().Code);
        Assert.AreEqual(1, inventory.Single().Amount);
        Assert.AreEqual("2", inventory.Single().Quality);
    }

    [TestMethod]
    public void ConfirmedEmptyInventoryIsARealCacheUpdate()
    {
        var harness = CreateHarness(CompanionCapability.Inventory);

        var output = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.InventorySnapshot,
                sequence: 2,
                new CompanionInventoryReport(Array.Empty<CompanionInventoryStack>())),
            Now);

        Assert.IsInstanceOfType<CompanionInventoryUpdated>(output);
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            harness.Inventory.TryGet(AuthoritativePlayer.GameId, Now, out var inventory));
        Assert.AreEqual(0, inventory.Count);
    }

    [TestMethod]
    public void PlayerDeathEmitsExactlyOnceWithTakaroSchema()
    {
        var harness = CreateHarness(CompanionCapability.PlayerDeath);
        var report = new CompanionPlayerDeathReport(
            "death-1",
            ToUnixMilliseconds("2026-07-11T11:59:57.123+00:00"),
            new CompanionPosition(10.5f, 35, -5.25f),
            "fall damage",
            "Deathsquito");

        var first = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.PlayerDeath, sequence: 2, report),
            Now);
        var duplicate = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.PlayerDeath, sequence: 3, report),
            Now);

        Assert.IsInstanceOfType<CompanionAcceptedEvent>(first);
        var accepted = (CompanionAcceptedEvent)first!;
        Assert.AreEqual(ValheimEventType.PlayerDeath, accepted.Type);
        Assert.IsNull(duplicate);
        using var document = Serialize(accepted);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");
        Assert.AreEqual("Steam_real", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("2026-07-11T11:59:57.123+00:00", data.GetProperty("timestamp").GetString());
        Assert.AreEqual(10.5, data.GetProperty("position").GetProperty("x").GetDouble());
        Assert.AreEqual(35, data.GetProperty("position").GetProperty("y").GetDouble());
        Assert.AreEqual(-5.25, data.GetProperty("position").GetProperty("z").GetDouble());
        Assert.AreEqual("valheim", data.GetProperty("position").GetProperty("dimension").GetString());
        StringAssert.Contains(data.GetProperty("msg").GetString(), "fall damage");
        StringAssert.Contains(data.GetProperty("msg").GetString(), "Deathsquito");
        Assert.IsFalse(data.TryGetProperty("attacker", out _));
    }

    [TestMethod]
    public void EntityKilledUsesPlayerEntityWeaponAndTimestampShape()
    {
        var harness = CreateHarness(CompanionCapability.EntityKilled);
        var report = new CompanionEntityKilledReport(
            "kill-1",
            ToUnixMilliseconds("2026-07-11T11:59:56.456+00:00"),
            new CompanionPosition(1, 2, 3),
            "Greydwarf",
            "SwordIron");

        var output = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.EntityKilled, sequence: 2, report),
            Now);

        Assert.IsInstanceOfType<CompanionAcceptedEvent>(output);
        var accepted = (CompanionAcceptedEvent)output!;
        Assert.AreEqual(ValheimEventType.EntityKilled, accepted.Type);
        using var document = Serialize(accepted);
        var data = document.RootElement.GetProperty("payload").GetProperty("data");
        Assert.AreEqual("Steam_real", data.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Greydwarf", data.GetProperty("entity").GetString());
        Assert.AreEqual("SwordIron", data.GetProperty("weapon").GetString());
        Assert.AreEqual("2026-07-11T11:59:56.456+00:00", data.GetProperty("timestamp").GetString());
        CollectionAssert.AreEquivalent(
            new[] { "player", "entity", "weapon", "timestamp" },
            data.EnumerateObject().Select(property => property.Name).ToArray());
    }

    [TestMethod]
    public void InvalidStaleDuplicateAndRateLimitedReportsEmitNothing()
    {
        var unknownPeerHarness = CreateHarness(CompanionCapability.Chat, rateLimitCapacity: 1);
        var unknownPeer = unknownPeerHarness.Processor.Process(
            peerId: 999,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 2,
                new CompanionChatReport("unknown", Now.ToUnixTimeMilliseconds(), "ignored")),
            Now);
        Assert.IsNull(unknownPeer);

        var harness = CreateHarness(CompanionCapability.Chat, rateLimitCapacity: 2);
        var stale = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 1,
                new CompanionChatReport("stale", Now.ToUnixTimeMilliseconds(), "ignored")),
            Now);
        Assert.IsNull(stale);

        var report = new CompanionChatReport("chat-once", Now.ToUnixTimeMilliseconds(), "once");
        var accepted = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.Chat, sequence: 2, report),
            Now);
        var duplicate = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.Chat, sequence: 3, report),
            Now);
        var rateLimited = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 4,
                new CompanionChatReport("chat-unique", Now.ToUnixTimeMilliseconds(), "ignored")),
            Now);

        Assert.IsInstanceOfType<CompanionAcceptedEvent>(accepted);
        Assert.IsNull(duplicate);
        Assert.IsNull(rateLimited);
    }

    [TestMethod]
    public void CapabilityMismatchMalformedWrongPayloadAndControlEmitNothing()
    {
        var harness = CreateHarness(CompanionCapability.Inventory);
        var capabilityMismatch = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 2,
                new CompanionChatReport("chat-1", Now.ToUnixTimeMilliseconds(), "ignored")),
            Now);
        var sequenceConsumedByMismatch = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.InventorySnapshot,
                sequence: 2,
                new CompanionInventoryReport(Array.Empty<CompanionInventoryStack>())),
            Now);
        var malformed = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            RawEnvelope(
                CompanionMessageTypes.InventorySnapshot,
                sequence: 3,
                """{"stacks":[],"unexpected":true}"""),
            Now);
        var wrongPayloadType = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            RawEnvelope(
                CompanionMessageTypes.InventorySnapshot,
                sequence: 4,
                """{"eventId":"chat-2","timestampUnixMilliseconds":1,"message":"wrong"}"""),
            Now);
        var control = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Heartbeat,
                sequence: 5,
                new CompanionHeartbeat(Now.ToUnixTimeMilliseconds())),
            Now);

        Assert.IsNull(capabilityMismatch);
        Assert.IsNull(sequenceConsumedByMismatch);
        Assert.IsNull(malformed);
        Assert.IsNull(wrongPayloadType);
        Assert.IsNull(control);
        Assert.AreEqual(
            CompanionInventoryState.Missing,
            harness.Inventory.TryGet(AuthoritativePlayer.GameId, Now, out _));
    }

    [TestMethod]
    public void PayloadCannotSupplyPlayerIdentity()
    {
        var harness = CreateHarness(CompanionCapability.Chat);
        var reportWithIdentity = RawEnvelope(
            CompanionMessageTypes.Chat,
            sequence: 2,
            """
            {
              "eventId":"chat-forged",
              "timestampUnixMilliseconds":1,
              "message":"forged",
              "player":{"gameId":"Steam_forged","name":"Loki"}
            }
            """);

        var forged = harness.Processor.Process(PeerId, AuthoritativePlayer, reportWithIdentity, Now);
        var valid = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 3,
                new CompanionChatReport("chat-forged", 1, "authoritative")),
            Now);

        Assert.IsNull(forged);
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(valid);
        var accepted = (CompanionAcceptedEvent)valid!;
        using var document = Serialize(accepted);
        Assert.AreEqual(
            "Steam_real",
            document.RootElement
                .GetProperty("payload")
                .GetProperty("data")
                .GetProperty("player")
                .GetProperty("gameId")
                .GetString());
    }

    [DataTestMethod]
    [DataRow(null, "SwordIron")]
    [DataRow(" ", "SwordIron")]
    [DataRow("Greydwarf", null)]
    [DataRow("Greydwarf", " ")]
    public void MissingRequiredEntityOrWeaponEmitsNothingAndDoesNotConsumeDeduplicationKey(
        string? entityCodeHint,
        string? weaponCodeHint)
    {
        var harness = CreateHarness(CompanionCapability.EntityKilled);
        var incomplete = new CompanionEntityKilledReport(
            "kill-retry",
            Now.ToUnixTimeMilliseconds(),
            new CompanionPosition(1, 2, 3),
            entityCodeHint,
            weaponCodeHint);
        var complete = incomplete with
        {
            EntityCodeHint = "Greydwarf",
            WeaponCodeHint = "SwordIron"
        };

        var rejected = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.EntityKilled, sequence: 2, incomplete),
            Now);
        var accepted = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.EntityKilled, sequence: 3, complete),
            Now);

        Assert.IsNull(rejected);
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(accepted);
    }

    [TestMethod]
    public void ReconnectScopesEventDeduplicationToNegotiatedNonce()
    {
        const string replacementNonce = "server-session-new";
        var harness = CreateHarness(CompanionCapability.Chat);
        var reusedEvent = new CompanionChatReport(
            "event-reused-across-sessions",
            Now.ToUnixTimeMilliseconds(),
            "old session");

        var oldAccepted = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.Chat, sequence: 2, reusedEvent),
            Now);

        harness.Sessions.Begin(PeerId, Now.AddSeconds(1), replacementNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            harness.Sessions.CompleteHelloAck(
                PeerId,
                replacementNonce,
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Chat,
                sequence: 1,
                Now.AddSeconds(2)));

        var staleAttempt = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 3,
                new CompanionChatReport(
                    "stale-session-event",
                    Now.ToUnixTimeMilliseconds(),
                    "stale")),
            Now.AddSeconds(3));
        var replacementAccepted = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(CompanionMessageTypes.Chat, sequence: 2, reusedEvent) with
            {
                SessionNonce = replacementNonce
            },
            Now.AddSeconds(3));
        var staleIdAcceptedInReplacement = harness.Processor.Process(
            PeerId,
            AuthoritativePlayer,
            Envelope(
                CompanionMessageTypes.Chat,
                sequence: 3,
                new CompanionChatReport(
                    "stale-session-event",
                    Now.ToUnixTimeMilliseconds(),
                    "current")) with
            {
                SessionNonce = replacementNonce
            },
            Now.AddSeconds(3));

        Assert.IsInstanceOfType<CompanionAcceptedEvent>(oldAccepted);
        Assert.IsNull(staleAttempt);
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(replacementAccepted);
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(staleIdAcceptedInReplacement);
    }

    private static Harness CreateHarness(
        CompanionCapability capabilities,
        int rateLimitCapacity = 10)
    {
        var sessions = new CompanionSessionRegistry(
            CompanionProtocol.CurrentVersion,
            CompanionProtocol.CurrentVersion,
            CompanionCapability.Chat
                | CompanionCapability.Inventory
                | CompanionCapability.PlayerDeath
                | CompanionCapability.EntityKilled,
            TimeSpan.FromSeconds(5),
            TimeSpan.FromMinutes(1));
        sessions.Begin(PeerId, Now, SessionNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            sessions.CompleteHelloAck(
                PeerId,
                SessionNonce,
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                capabilities,
                sequence: 1,
                Now));

        var inventory = new CompanionInventoryCache();
        inventory.BeginSession(PeerId, SessionNonce);
        var processor = new CompanionReportProcessor(
            sessions,
            new CompanionRateLimiter(
                rateLimitCapacity,
                refillTokens: 1,
                TimeSpan.FromMinutes(1)),
            new BoundedEventDeduplicator(capacity: 32),
            inventory);
        return new Harness(processor, sessions, inventory);
    }

    private static CompanionEnvelope Envelope<T>(string type, long sequence, T payload)
    {
        var payloadElement = JsonSerializer.SerializeToElement(
            payload,
            new JsonSerializerOptions(JsonSerializerDefaults.Web));
        return new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            SessionNonce,
            sequence,
            $"message-{sequence}",
            type,
            payloadElement);
    }

    private static CompanionEnvelope RawEnvelope(string type, long sequence, string payloadJson)
    {
        using var document = JsonDocument.Parse(payloadJson);
        return new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            SessionNonce,
            sequence,
            $"message-{sequence}",
            type,
            document.RootElement.Clone());
    }

    private static JsonDocument Serialize(CompanionAcceptedEvent accepted) =>
        JsonDocument.Parse(TakaroProtocol.CreateGameEvent(accepted.Type, accepted.Data));

    private static long ToUnixMilliseconds(string timestamp) =>
        DateTimeOffset.Parse(timestamp).ToUnixTimeMilliseconds();

    private sealed record Harness(
        CompanionReportProcessor Processor,
        CompanionSessionRegistry Sessions,
        CompanionInventoryCache Inventory);
}
