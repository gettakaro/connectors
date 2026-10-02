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
            CompanionMessageTypes.InventorySnapshot,
            EmptyInventory(),
            out _));
        Assert.IsTrue(state.TryPrepareHelloAck(
            Hello("nonce-a"),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsFalse(state.CanReport);
        Assert.IsFalse(state.TryCreateReport(
            CompanionMessageTypes.InventorySnapshot,
            EmptyInventory(),
            out _));

        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));
        Assert.IsTrue(state.CanReport);
        Assert.IsTrue(state.TryCreateReport(
            CompanionMessageTypes.KillVerdict,
            Verdict(),
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
            capabilities: 63);

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
        Assert.AreEqual(CompanionCapability.Inventory, ack.AcceptedCapabilities);
    }

    [TestMethod]
    public void ClientReportsIncompatibleRangeWithoutBecomingReady()
    {
        var state = new CompanionClientState(
            minimumProtocolVersion: 2,
            maximumProtocolVersion: 3,
            CompanionCapability.Inventory);

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
            CompanionMessageTypes.InventorySnapshot,
            EmptyInventory(),
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
            Hello("nonce-a", capabilities: 0),
            "1.2.3",
            out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));

        Assert.IsTrue(state.TryCreateReport(
            CompanionMessageTypes.KillVerdict,
            Verdict(),
            out _));
        Assert.IsFalse(state.TryCreateReport(
            CompanionMessageTypes.InventorySnapshot,
            EmptyInventory(),
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
    public void ClientAnswersALegacyProtocolTwoServerHelloWithANackItCanRead()
    {
        var state = CreateState();
        var legacyHello = Hello("legacy", minimumVersion: 2, maximumVersion: 2, capabilities: 63, envelopeVersion: 2);

        Assert.IsTrue(state.TryPrepareHelloAck(legacyHello, "3.1.0", out var prepared));
        Assert.IsNotNull(prepared);
        Assert.AreEqual(CompanionMessageTypes.HelloNack, prepared.Envelope.Type);
        Assert.AreEqual(2, prepared.Envelope.ProtocolVersion);
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloNack>(prepared.Envelope, out var nack, out _));
        Assert.IsNotNull(nack);
        Assert.AreEqual(CompanionProtocol.MinimumVersion, nack.MinimumVersion);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, nack.MaximumVersion);
        Assert.IsFalse(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));
        Assert.IsFalse(state.CanReport);
    }

    [TestMethod]
    public void ClientAcceptsTheServersProtocolThreeHelloInANegotiationEnvelope()
    {
        var state = CreateState();
        var hello = Hello(
            "nonce-v3",
            capabilities: (int)CompanionCapability.Inventory,
            envelopeVersion: CompanionProtocol.NegotiationEnvelopeVersion);

        Assert.IsTrue(state.TryPrepareHelloAck(hello, "3.1.0", out var prepared));
        Assert.IsNotNull(prepared);
        Assert.AreEqual(CompanionMessageTypes.HelloAck, prepared.Envelope.Type);
        Assert.AreEqual(3, prepared.Envelope.ProtocolVersion);
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));
        Assert.IsTrue(state.HasCapability(CompanionCapability.Inventory));
    }

    [TestMethod]
    public void KillVerdictNeedsOnlyANegotiatedSessionWhileInventoryNeedsItsCapability()
    {
        var state = CreateState();
        Assert.IsTrue(state.TryPrepareHelloAck(Hello("no-inventory", capabilities: 0), "3.1.0", out var prepared));
        Assert.IsNotNull(prepared);
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared, TimeSpan.Zero));

        Assert.IsFalse(state.TryCreateReport(CompanionMessageTypes.InventorySnapshot, EmptyInventory(), out _));
        Assert.IsTrue(state.TryCreateReport(CompanionMessageTypes.KillVerdict, Verdict(), out var verdict));
        Assert.IsNotNull(verdict);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, verdict.ProtocolVersion);
        Assert.IsFalse(state.TryCreateReport(CompanionMessageTypes.Heartbeat, new CompanionHeartbeat(0), out _));
    }

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
        int capabilities = (int)CompanionCapability.Inventory,
        int envelopeVersion = CompanionProtocol.NegotiationEnvelopeVersion) =>
        new(
            envelopeVersion,
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

    private static CompanionInventoryReport EmptyInventory() =>
        new(Array.Empty<CompanionInventoryStack>());

    private static CompanionKillVerdict Verdict() =>
        new("123456:42", "Boar", "$enemy_boar", true, CompanionAttackerKind.LocalPlayer);

    private static CompanionClientState CreateState() =>
        new(CompanionProtocol.MinimumVersion, CompanionProtocol.CurrentVersion, CompanionCapability.Inventory);
}
