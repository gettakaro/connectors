using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionClientStateTests
{
    private static readonly DateTimeOffset UtcNow =
        DateTimeOffset.Parse("2026-07-11T12:00:00+00:00");

    [TestMethod]
    public void ClientCannotReportBeforeHello()
    {
        var state = CreateState();

        Assert.IsFalse(state.TryCreateReport(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("too-early", UtcNow.ToUnixTimeMilliseconds(), "ignored"),
            out _));
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-a"),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsFalse(state.CanReport);
        Assert.IsFalse(state.TryCreateReport(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("still-early", UtcNow.ToUnixTimeMilliseconds(), "ignored"),
            out _));

        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));
        Assert.IsTrue(state.CanReport);
        Assert.IsTrue(state.TryCreateReport(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("ready", UtcNow.ToUnixTimeMilliseconds(), "hello"),
            out _));
    }

    [TestMethod]
    public void ClientEchoesNonceAndSelectsHighestCompatibleVersion()
    {
        var state = CreateState();
        var hello = Hello(
            "nonce-from-server",
            minimumVersion: CompanionProtocol.MinimumVersion,
            maximumVersion: CompanionProtocol.CurrentVersion + 3,
            capabilities: CompanionCapability.Chat
                | CompanionCapability.Inventory
                | CompanionCapability.PlayerDeath);

        Assert.IsTrue(state.TryPrepareHelloAck(
            hello,
            "1.2.3+client",
            out var prepared));
        Assert.IsNotNull(prepared);
        var envelope = prepared.Envelope;
        Assert.AreEqual("nonce-from-server", envelope.SessionNonce);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, envelope.ProtocolVersion);
        Assert.AreEqual(1, envelope.Sequence);
        Assert.AreEqual(CompanionMessageTypes.HelloAck, envelope.Type);
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(
            envelope,
            out var ack,
            out _));
        Assert.IsNotNull(ack);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, ack.ProtocolVersion);
        Assert.AreEqual("1.2.3+client", ack.ProductVersion);
        Assert.AreEqual(
            CompanionCapability.Chat
                | CompanionCapability.Inventory
                | CompanionCapability.PlayerDeath,
            ack.AcceptedCapabilities);
    }

    [TestMethod]
    public void NegotiationEnvelopeUsesOldestAdvertisedVersionForForwardCompatibility()
    {
        Assert.AreEqual(
            1,
            CompanionVersionPolicy.SelectNegotiationEnvelopeVersion(
                minimumVersion: 1,
                currentVersion: 2));
    }

    [TestMethod]
    public void ClientReportsIncompatibleRangeWithoutBecomingReady()
    {
        var state = new CompanionClientState(
            minimumProtocolVersion: 2,
            maximumProtocolVersion: 3,
            CompanionCapability.Chat);

        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("incompatible", minimumVersion: 1, maximumVersion: 1),
            "3.0.0-client",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.AreEqual(CompanionMessageTypes.HelloNack, prepared.Envelope.Type);
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloNack>(
            prepared.Envelope,
            out var nack,
            out _));
        Assert.IsNotNull(nack);
        Assert.AreEqual(2, nack.MinimumVersion);
        Assert.AreEqual(3, nack.MaximumVersion);
        Assert.AreEqual("3.0.0-client", nack.ProductVersion);
        Assert.IsFalse(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));
        Assert.IsFalse(state.CanReport);
    }

    [TestMethod]
    public void ClientSequenceIsStrictlyMonotonicWithinSession()
    {
        var state = CreateState();
        var prepared = PrepareAndConfirm(state, "nonce-a");

        Assert.AreEqual(1, prepared.Envelope.Sequence);
        Assert.IsTrue(state.TryCreateHeartbeat(
            CompanionClientState.HeartbeatInterval,
            UtcNow,
            out var heartbeat));
        Assert.IsNotNull(heartbeat);
        Assert.AreEqual(2, heartbeat.Sequence);
        Assert.IsTrue(state.TryCreateReport(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("chat-1", UtcNow.ToUnixTimeMilliseconds(), "hello"),
            out var report));
        Assert.IsNotNull(report);
        Assert.AreEqual(3, report.Sequence);
    }

    [TestMethod]
    public void ClientHeartbeatUsesExactFiveSecondInterval()
    {
        var state = CreateState();
        _ = PrepareAndConfirm(state, "nonce-a", TimeSpan.FromSeconds(10));

        Assert.IsFalse(state.TryCreateHeartbeat(
            TimeSpan.FromSeconds(15) - TimeSpan.FromTicks(1),
            UtcNow,
            out _));
        Assert.IsTrue(state.TryCreateHeartbeat(
            TimeSpan.FromSeconds(15),
            UtcNow,
            out var first));
        Assert.IsNotNull(first);
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHeartbeat>(
            first,
            out var payload,
            out _));
        Assert.IsNotNull(payload);
        Assert.AreEqual(UtcNow.ToUnixTimeMilliseconds(), payload.TimestampUnixMilliseconds);
        Assert.IsFalse(state.TryCreateHeartbeat(
            TimeSpan.FromSeconds(20) - TimeSpan.FromTicks(1),
            UtcNow.AddSeconds(5),
            out _));
        Assert.IsTrue(state.TryCreateHeartbeat(
            TimeSpan.FromSeconds(20),
            UtcNow.AddSeconds(5),
            out _));
    }

    [TestMethod]
    public void ClientResetClearsNonceSequenceAndReportReadiness()
    {
        var state = CreateState();
        _ = PrepareAndConfirm(state, "nonce-a");
        Assert.IsTrue(state.CanReport);

        state.Reset();

        Assert.IsFalse(state.CanReport);
        Assert.IsNull(state.SessionNonce);
        Assert.IsFalse(state.TryCreateHeartbeat(
            TimeSpan.MaxValue,
            UtcNow,
            out _));
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-b"),
            "1.2.3",
            out var replacement));
        Assert.IsNotNull(replacement);
        Assert.AreEqual(1, replacement.Envelope.Sequence);
    }

    [TestMethod]
    public void ClientRejectsStaleSessionHelloAndConfirmation()
    {
        var state = CreateState();
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-a"),
            "1.2.3",
            out var first));
        Assert.IsNotNull(first);
        Assert.IsTrue(state.ConfirmHelloAckSent(first, TimeSpan.Zero));
        Assert.IsFalse(state.TryPrepareHelloAck(
            Hello("nonce-a"),
            "1.2.3",
            out _));
        state.Reset();

        Assert.IsFalse(state.TryPrepareHelloAck(
            Hello("nonce-a"),
            "1.2.3",
            out _));
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-b"),
            "1.2.3",
            out var replacement));
        Assert.IsNotNull(replacement);
        Assert.IsFalse(state.ConfirmHelloAckSent(first, TimeSpan.Zero));
        Assert.IsTrue(state.ConfirmHelloAckSent(replacement, TimeSpan.Zero));
        Assert.AreEqual("nonce-b", state.SessionNonce);
    }

    [TestMethod]
    public void ClientReportsOnlyNegotiatedCapabilitiesAndDoesNotBurstHeartbeats()
    {
        var state = CreateState();
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-a", capabilities: CompanionCapability.Chat),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));

        Assert.IsTrue(state.TryCreateReport(
            CompanionMessageTypes.Chat,
            new CompanionChatReport("chat-1", UtcNow.ToUnixTimeMilliseconds(), "hello"),
            out _));
        Assert.IsFalse(state.TryCreateReport(
            CompanionMessageTypes.InventorySnapshot,
            new CompanionInventoryReport(Array.Empty<CompanionInventoryStack>()),
            out _));
        Assert.IsTrue(state.TryCreateHeartbeat(
            TimeSpan.FromMinutes(1),
            UtcNow,
            out _));
        Assert.IsFalse(state.TryCreateHeartbeat(
            TimeSpan.FromMinutes(1) + TimeSpan.FromTicks(1),
            UtcNow,
            out _));
        Assert.IsTrue(state.TryCreateHeartbeat(
            TimeSpan.FromMinutes(1) + CompanionClientState.HeartbeatInterval,
            UtcNow.AddSeconds(5),
            out _));
    }

    [TestMethod]
    public void ClientAcceptsOnlyCurrentMonotonicServerChat()
    {
        var state = CreateState();
        var chat = ServerChat("nonce-a", sequence: 2, "Hello");

        Assert.IsFalse(state.TryAcceptServerChat(chat, out _), "Pre-negotiation chat must be rejected.");
        _ = PrepareAndConfirm(state, "nonce-a");

        Assert.IsTrue(state.TryAcceptServerChat(chat, out var accepted));
        Assert.IsNotNull(accepted);
        Assert.AreEqual("Takaro", accepted.Sender);
        Assert.AreEqual("Hello", accepted.Message);
        Assert.IsFalse(state.TryAcceptServerChat(chat, out _), "A replayed sequence must be rejected.");
        Assert.IsFalse(state.TryAcceptServerChat(ServerChat("wrong", 3, "Wrong nonce"), out _));
        Assert.IsFalse(state.TryAcceptServerChat(ServerChat("nonce-a", 3, "Wrong version") with
        {
            ProtocolVersion = CompanionProtocol.CurrentVersion + 1
        }, out _));
        Assert.IsTrue(state.TryAcceptServerChat(ServerChat("nonce-a", 3, "Next"), out _));

        state.Reset();
        Assert.IsFalse(state.TryAcceptServerChat(ServerChat("nonce-a", 4, "Retired"), out _));
    }

    [TestMethod]
    public void ClientRejectsServerChatWithoutNegotiatedCapability()
    {
        var state = CreateState();
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-a", capabilities: CompanionCapability.Chat),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));

        Assert.IsFalse(state.TryAcceptServerChat(ServerChat("nonce-a", 2, "Ignored"), out _));
    }

    private static CompanionClientState CreateState() =>
        new(
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            CompanionCapability.Chat
                | CompanionCapability.Inventory
                | CompanionCapability.PlayerDeath
                | CompanionCapability.EntityKilled
                | CompanionCapability.ServerChat);

    private static PreparedCompanionHelloAck PrepareAndConfirm(
        CompanionClientState state,
        string nonce,
        TimeSpan? monotonicNow = null)
    {
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello(nonce),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsTrue(state.ConfirmHelloAckSent(
            prepared,
            monotonicNow ?? TimeSpan.Zero));
        return prepared;
    }

    private static CompanionEnvelope Hello(
        string nonce,
        int minimumVersion = CompanionProtocol.MinimumVersion,
        int maximumVersion = CompanionProtocol.CurrentVersion,
        CompanionCapability capabilities = CompanionCapability.Chat
            | CompanionCapability.Inventory
            | CompanionCapability.PlayerDeath
            | CompanionCapability.EntityKilled
            | CompanionCapability.ServerChat) =>
        new(
            CompanionProtocol.CurrentVersion,
            nonce,
            1,
            $"hello-{nonce}",
            CompanionMessageTypes.Hello,
            JsonSerializer.SerializeToElement(new CompanionHello(
                minimumVersion,
                maximumVersion,
                capabilities),
                new JsonSerializerOptions
                {
                    PropertyNamingPolicy = JsonNamingPolicy.CamelCase
                }));

    private static CompanionEnvelope ServerChat(
        string nonce,
        long sequence,
        string message) =>
        new(
            CompanionProtocol.CurrentVersion,
            nonce,
            sequence,
            $"server-{sequence}",
            CompanionMessageTypes.ServerChat,
            JsonSerializer.SerializeToElement(
                new CompanionServerChatMessage("Takaro", message),
                new JsonSerializerOptions
                {
                    PropertyNamingPolicy = JsonNamingPolicy.CamelCase
                }));
}
