using System.Collections.Concurrent;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionSessionRegistryTests
{
    private const long PeerId = 7_654_321;
    private const string CurrentNonce = "nonce-current";
    private const string ProductVersion = "1.2.3";
    private const int MinimumProtocolVersion = 1;
    private const int MaximumProtocolVersion = 2;

    private static readonly DateTimeOffset Now =
        DateTimeOffset.Parse("2026-07-11T10:00:00+02:00");

    private static readonly TimeSpan HandshakeGrace = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan HeartbeatGrace = TimeSpan.FromSeconds(30);
    private static readonly CompanionCapability SupportedCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled;

    [TestMethod]
    public void ReportBeforeHelloAckIsRejected()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        var before = Snapshot(registry, PeerId);

        var decision = registry.ValidateReport(
            PeerId,
            CurrentNonce,
            protocolVersion: 1,
            sequence: 1,
            Now.AddSeconds(1));

        Assert.AreEqual(CompanionSessionDecision.RejectNotNegotiated, decision);
        Assert.AreEqual(before, Snapshot(registry, PeerId));
        Assert.AreEqual(0, before.LastSequence);
        Assert.IsNull(before.LastHeartbeat);
    }

    [TestMethod]
    public void HelloAckMustEchoCurrentNonce()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        var pending = Snapshot(registry, PeerId);

        var rejected = registry.CompleteHelloAck(
            PeerId,
            "nonce-from-another-session",
            selectedProtocolVersion: 1,
            ProductVersion,
            CompanionCapability.Chat,
            sequence: 1,
            Now.AddSeconds(1));

        Assert.AreEqual(CompanionSessionDecision.RejectNonce, rejected);
        Assert.AreEqual(pending, Snapshot(registry, PeerId));

        var accepted = registry.CompleteHelloAck(
            PeerId,
            CurrentNonce,
            selectedProtocolVersion: 1,
            ProductVersion,
            CompanionCapability.Chat,
            sequence: 1,
            Now.AddSeconds(1));

        Assert.AreEqual(CompanionSessionDecision.Accept, accepted);
        Assert.IsTrue(Snapshot(registry, PeerId).IsNegotiated);
    }

    [TestMethod]
    public void ReconnectReplacesNonceAndRejectsOldSession()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, "nonce-old");
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, "nonce-old", sequence: 1, Now.AddSeconds(1)));

        var begin = registry.Begin(PeerId, Now.AddSeconds(2), "nonce-new");

        Assert.AreEqual(PeerId, begin.PeerId);
        Assert.AreEqual("nonce-new", begin.Nonce);
        Assert.AreEqual(Now.AddSeconds(2) + HandshakeGrace, begin.HandshakeDeadline);
        var replacement = Snapshot(registry, PeerId);
        Assert.AreEqual("nonce-new", replacement.Nonce);
        Assert.IsFalse(replacement.IsNegotiated);
        Assert.AreEqual(0, replacement.LastSequence);
        Assert.IsNull(replacement.LastHeartbeat);

        Assert.AreEqual(
            CompanionSessionDecision.RejectNonce,
            registry.ValidateReport(PeerId, "nonce-old", 1, 2, Now.AddSeconds(3)));
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, "nonce-new", sequence: 1, Now.AddSeconds(3)));
        Assert.AreEqual(
            CompanionSessionDecision.RejectNonce,
            registry.ValidateHeartbeat(PeerId, "nonce-old", 1, 2, Now.AddSeconds(4)));
    }

    [TestMethod]
    public void SequenceMustIncreaseWithinSession()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 5, Now.AddSeconds(1)));
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 6, Now.AddSeconds(2)));
        var acceptedSnapshot = Snapshot(registry, PeerId);

        Assert.AreEqual(
            CompanionSessionDecision.RejectSequence,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 6, Now.AddSeconds(3)));
        Assert.AreEqual(
            CompanionSessionDecision.RejectSequence,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 4, Now.AddSeconds(3)));
        Assert.AreEqual(acceptedSnapshot, Snapshot(registry, PeerId));

        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 7, Now.AddSeconds(4)));
        Assert.AreEqual(6, acceptedSnapshot.LastSequence, "Previously returned snapshots must stay immutable.");
        Assert.AreEqual(7, Snapshot(registry, PeerId).LastSequence);
    }

    [TestMethod]
    public void HeartbeatRefreshesOnlyNegotiatedSession()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        var pending = Snapshot(registry, PeerId);

        Assert.AreEqual(
            CompanionSessionDecision.RejectNotNegotiated,
            registry.ValidateHeartbeat(PeerId, CurrentNonce, 1, 1, Now.AddSeconds(1)));
        Assert.AreEqual(pending, Snapshot(registry, PeerId));

        var negotiatedAt = Now.AddSeconds(2);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 1, negotiatedAt));
        var negotiated = Snapshot(registry, PeerId);
        Assert.AreEqual(negotiatedAt, negotiated.LastHeartbeat);

        Assert.AreEqual(
            CompanionSessionDecision.RejectNonce,
            registry.ValidateHeartbeat(PeerId, "wrong-nonce", 1, 2, Now.AddSeconds(3)));
        Assert.AreEqual(negotiated, Snapshot(registry, PeerId));

        var heartbeatAt = Now.AddSeconds(4);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            registry.ValidateHeartbeat(PeerId, CurrentNonce, 1, 2, heartbeatAt));
        var refreshed = Snapshot(registry, PeerId);
        Assert.AreEqual(heartbeatAt, refreshed.LastHeartbeat);
        Assert.AreEqual(2, refreshed.LastSequence);
        Assert.AreEqual(heartbeatAt + HeartbeatGrace, refreshed.ExpiresAt);
    }

    [TestMethod]
    public void RequiredSessionExpiresAfterHandshakeGrace()
    {
        var registry = CreateRegistry();
        var beforeBoundaryPeer = PeerId;
        var atBoundaryPeer = PeerId + 1;
        registry.Begin(beforeBoundaryPeer, Now, "nonce-before-boundary");
        registry.Begin(atBoundaryPeer, Now, "nonce-at-boundary");

        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(
                registry,
                beforeBoundaryPeer,
                "nonce-before-boundary",
                sequence: 1,
                Now + HandshakeGrace - TimeSpan.FromTicks(1)));

        var pending = Snapshot(registry, atBoundaryPeer);
        Assert.AreEqual(
            CompanionSessionDecision.Expired,
            Negotiate(
                registry,
                atBoundaryPeer,
                "nonce-at-boundary",
                sequence: 1,
                Now + HandshakeGrace));
        Assert.AreEqual(pending, Snapshot(registry, atBoundaryPeer));
    }

    [TestMethod]
    public void HeartbeatExpiresAfterGrace()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 1, Now));

        var justBeforeExpiry = Now + HeartbeatGrace - TimeSpan.FromTicks(1);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            registry.ValidateHeartbeat(PeerId, CurrentNonce, 1, 2, justBeforeExpiry));
        var refreshed = Snapshot(registry, PeerId);

        Assert.AreEqual(
            CompanionSessionDecision.Expired,
            registry.ValidateHeartbeat(
                PeerId,
                CurrentNonce,
                1,
                3,
                justBeforeExpiry + HeartbeatGrace));
        Assert.AreEqual(refreshed, Snapshot(registry, PeerId));
    }

    [TestMethod]
    public void RemovePeerAndSwitchWorldClearSessionState()
    {
        var registry = CreateRegistry();
        var firstWorld = new string("world-a".ToCharArray());
        var equivalentFirstWorld = new string("world-a".ToCharArray());
        registry.SwitchWorld(firstWorld);
        registry.Begin(PeerId, Now, CurrentNonce);

        registry.RemovePeer(PeerId);

        Assert.IsFalse(registry.TryGetSnapshot(PeerId, out _));
        Assert.AreEqual(
            CompanionSessionDecision.RejectUnknownPeer,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 1, Now));

        registry.Begin(PeerId, Now, CurrentNonce);
        registry.Begin(PeerId + 1, Now, "nonce-second-peer");
        registry.SwitchWorld(equivalentFirstWorld);

        Assert.IsTrue(registry.TryGetSnapshot(PeerId, out _), "Value-equivalent world identity must not clear state.");
        Assert.IsTrue(registry.TryGetSnapshot(PeerId + 1, out _));

        registry.SwitchWorld("world-b");

        Assert.IsFalse(registry.TryGetSnapshot(PeerId, out _));
        Assert.IsFalse(registry.TryGetSnapshot(PeerId + 1, out _));
    }

    [TestMethod]
    public void UnknownPeerOperationsAreRejected()
    {
        var registry = CreateRegistry();

        Assert.AreEqual(
            CompanionSessionDecision.RejectUnknownPeer,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 1, Now));
        Assert.AreEqual(
            CompanionSessionDecision.RejectUnknownPeer,
            registry.ValidateReport(PeerId, CurrentNonce, 1, 1, Now));
        Assert.AreEqual(
            CompanionSessionDecision.RejectUnknownPeer,
            registry.ValidateHeartbeat(PeerId, CurrentNonce, 1, 1, Now));
        Assert.IsFalse(registry.TryGetSnapshot(PeerId, out _));
    }

    [TestMethod]
    public void HelloAckRejectsUnsupportedVersionInvalidProductAndCapabilitiesWithoutMutation()
    {
        var registry = CreateRegistry(supportedCapabilities: CompanionCapability.Chat);
        registry.Begin(PeerId, Now, CurrentNonce);
        var pending = Snapshot(registry, PeerId);

        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: MaximumProtocolVersion + 1,
            ProductVersion,
            CompanionCapability.Chat,
            sequence: 1,
            CompanionSessionDecision.RejectVersion);
        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: 1,
            "   ",
            CompanionCapability.Chat,
            sequence: 1,
            CompanionSessionDecision.RejectMetadata);
        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: 1,
            new string('v', CompanionSessionRegistry.MaximumProductVersionCharacters + 1),
            CompanionCapability.Chat,
            sequence: 1,
            CompanionSessionDecision.RejectMetadata);
        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: 1,
            ProductVersion,
            CompanionCapability.Inventory,
            sequence: 1,
            CompanionSessionDecision.RejectMetadata);
        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: 1,
            ProductVersion,
            (CompanionCapability)(1 << 20),
            sequence: 1,
            CompanionSessionDecision.RejectMetadata);
        AssertRejectedNegotiationDoesNotMutate(
            registry,
            pending,
            selectedProtocolVersion: 1,
            ProductVersion,
            CompanionCapability.Chat,
            sequence: 0,
            CompanionSessionDecision.RejectSequence);

        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 1, Now.AddSeconds(1)));
    }

    [TestMethod]
    public void RejectedReportsAndHeartbeatsDoNotMutateNegotiatedState()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 3, Now.AddSeconds(1)));
        var negotiated = Snapshot(registry, PeerId);

        Assert.AreEqual(
            CompanionSessionDecision.RejectVersion,
            registry.ValidateReport(PeerId, CurrentNonce, 2, 4, Now.AddSeconds(2)));
        Assert.AreEqual(
            CompanionSessionDecision.RejectNonce,
            registry.ValidateReport(PeerId, "wrong", 1, 4, Now.AddSeconds(2)));
        Assert.AreEqual(
            CompanionSessionDecision.RejectSequence,
            registry.ValidateHeartbeat(PeerId, CurrentNonce, 1, 3, Now.AddSeconds(2)));

        Assert.AreEqual(negotiated, Snapshot(registry, PeerId));
    }

    [TestMethod]
    public void ConcurrentCompoundOperationsRemainAtomic()
    {
        var registry = CreateRegistry();
        registry.Begin(PeerId, Now, CurrentNonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            Negotiate(registry, PeerId, CurrentNonce, sequence: 1, Now));
        var decisions = new ConcurrentBag<CompanionSessionDecision>();

        Parallel.For(2, 1_001, sequence =>
        {
            decisions.Add(registry.ValidateReport(PeerId, CurrentNonce, 1, sequence, Now.AddSeconds(1)));
        });

        Assert.IsTrue(decisions.Contains(CompanionSessionDecision.Accept));
        Assert.IsTrue(decisions.All(decision =>
            decision is CompanionSessionDecision.Accept or CompanionSessionDecision.RejectSequence));
        Assert.AreEqual(1_000, Snapshot(registry, PeerId).LastSequence);
    }

    private static CompanionSessionRegistry CreateRegistry(
        CompanionCapability? supportedCapabilities = null) =>
        new(
            MinimumProtocolVersion,
            MaximumProtocolVersion,
            supportedCapabilities ?? SupportedCapabilities,
            HandshakeGrace,
            HeartbeatGrace);

    private static CompanionSessionDecision Negotiate(
        CompanionSessionRegistry registry,
        long peerId,
        string nonce,
        long sequence,
        DateTimeOffset now) =>
        registry.CompleteHelloAck(
            peerId,
            nonce,
            selectedProtocolVersion: 1,
            ProductVersion,
            CompanionCapability.Chat,
            sequence,
            now);

    private static CompanionSessionSnapshot Snapshot(CompanionSessionRegistry registry, long peerId)
    {
        Assert.IsTrue(registry.TryGetSnapshot(peerId, out var snapshot));
        return snapshot;
    }

    private static void AssertRejectedNegotiationDoesNotMutate(
        CompanionSessionRegistry registry,
        CompanionSessionSnapshot expectedSnapshot,
        int selectedProtocolVersion,
        string productVersion,
        CompanionCapability capabilities,
        long sequence,
        CompanionSessionDecision expectedDecision)
    {
        var decision = registry.CompleteHelloAck(
            PeerId,
            CurrentNonce,
            selectedProtocolVersion,
            productVersion,
            capabilities,
            sequence,
            Now.AddSeconds(1));

        Assert.AreEqual(expectedDecision, decision);
        Assert.AreEqual(expectedSnapshot, Snapshot(registry, PeerId));
    }
}
