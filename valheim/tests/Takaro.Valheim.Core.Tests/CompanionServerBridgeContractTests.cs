using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionServerBridgeContractTests
{
    [TestMethod]
    public void DefaultHandshakeGraceAccommodatesSlowWorldLoads()
    {
        var bridge = ReadPluginSource("CompanionServerBridge.cs");

        StringAssert.Contains(
            bridge,
            "DefaultHandshakeGrace = TimeSpan.FromSeconds(30)");
        StringAssert.Contains(
            bridge,
            "DefaultHandshakeGrace,");
    }

    [TestMethod]
    public void ServerHelloUsesNegotiationEnvelopeVersionInsteadOfCurrentVersion()
    {
        var bridge = ReadPluginSource("CompanionServerBridge.cs");
        var beginSession = Slice(bridge, "private void BeginSession", "private void HandleEnvelope");

        StringAssert.Contains(
            beginSession,
            "CompanionVersionPolicy.SelectNegotiationEnvelopeVersion(");
        StringAssert.Contains(beginSession, "CompanionProtocol.MinimumVersion");
        StringAssert.Contains(beginSession, "CompanionProtocol.CurrentVersion");
    }

    private static readonly DateTimeOffset Now = new(2026, 7, 11, 12, 0, 0, TimeSpan.Zero);
    private static readonly JsonSerializerOptions WireJson = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };
    private const long PeerId = 42;
    private const string Nonce = "server-owned-nonce";

    [TestMethod]
    public void ServerRegistersOneBoundedEnvelopeRpc()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");
        var handler = ReadCoreSource("CompanionServerMessageHandler.cs");

        StringAssert.Contains(source, "Register<string>(CompanionProtocol.RpcName");
        StringAssert.Contains(handler, "CompanionEnvelopeCodec.TryDecodeEnvelope");
        StringAssert.Contains(source, "CompanionProtocol.MaximumEnvelopeUtf8Bytes");
        StringAssert.Contains(source, "ReferenceEquals(routedRpc, registeredRpc)");
        Assert.IsFalse(source.Contains("Register<ZPackage>", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerTargetsHelloToTheExactPeer()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");

        StringAssert.Contains(source, "InvokeRoutedRPC(");
        StringAssert.Contains(source, "peer.m_uid");
        StringAssert.Contains(source, "CompanionMessageTypes.Hello");
        Assert.IsFalse(source.Contains("ZRoutedRpc.Everybody", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("InvokeRoutedRPC(0", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerChatTargetsOnlyAnActiveNegotiatedExactPeer()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");
        var send = Slice(
            source,
            "public bool TrySendServerChat",
            "private void ForwardAcceptedEvent");

        StringAssert.Contains(source, "CompanionCapability.ServerChat");
        StringAssert.Contains(send, "sessions.TryGetActiveSession(");
        StringAssert.Contains(send, "peer.m_uid,");
        StringAssert.Contains(send, "CompanionCapability.ServerChat,");
        StringAssert.Contains(send, "MatchesTrackedPeer(peer.m_uid, peer)");
        StringAssert.Contains(send, "snapshot.SelectedProtocolVersion.Value");
        StringAssert.Contains(send, "snapshot.Nonce");
        StringAssert.Contains(send, "CompanionMessageTypes.ServerChat");
        StringAssert.Contains(send, "new CompanionServerChatMessage(\"Takaro\", message)");
        StringAssert.Contains(send, "routedRpc.InvokeRoutedRPC(");
        StringAssert.Contains(send, "peer.m_uid");
        StringAssert.Contains(send, "tracked.NextServerSequence++");
        Assert.IsTrue(
            send.IndexOf("InvokeRoutedRPC(", StringComparison.Ordinal)
            < send.IndexOf("tracked.NextServerSequence++", StringComparison.Ordinal));
        Assert.IsFalse(send.Contains("ZRoutedRpc.Everybody", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerBindsReportsToTheRpcSender()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");

        StringAssert.Contains(source, "TryResolveConnectedPeer(sender");
        StringAssert.Contains(source, "messageHandler.Handle(sender, player");
        Assert.IsFalse(source.Contains("payloadPlayer", StringComparison.OrdinalIgnoreCase));
        Assert.IsFalse(source.Contains("claimedPlayer", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void ServerRejectsUnknownOrNotReadyPeer()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");
        var handler = Slice(source, "private void HandleEnvelope", "private void ForwardAcceptedEvent");

        StringAssert.Contains(handler, "TryResolveConnectedPeer(sender");
        StringAssert.Contains(handler, "MatchesTrackedPeer(sender, peer)");
        StringAssert.Contains(handler, "return;");
        Assert.IsTrue(
            handler.IndexOf("TryResolveConnectedPeer(sender", StringComparison.Ordinal)
            < handler.IndexOf("messageHandler.Handle(sender, player", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerForwardsOnlyAcceptedEvents()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");
        var handler = Slice(source, "private void HandleEnvelope", "private void ForwardAcceptedEvent");

        StringAssert.Contains(handler, "result.Output is CompanionAcceptedEvent acceptedEvent");
        StringAssert.Contains(source, "runner.SendGameEventAsync(");
        StringAssert.Contains(source, "acceptedEvent.Type");
        StringAssert.Contains(source, "acceptedEvent.Data");
        StringAssert.Contains(source, "BoundedCompanionEventQueue");
        StringAssert.Contains(source, "CancellationTokenSource");
        Assert.IsFalse(source.Contains("CompanionInventoryUpdated inventory =>", StringComparison.Ordinal));
    }

    [TestMethod]
    public void WorldChangeAndDisconnectClearCompanionState()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");

        foreach (var marker in new[]
        {
            "GetWorldUID()",
            "CompanionEnforcementPolicy.Evaluate",
            "pendingEvents.AdvanceGeneration()",
            "sessions.SwitchWorld",
            "inventory.SwitchWorld",
            "sessions.RemovePeer",
            "inventory.RemovePeer",
            "rateLimiter.RemovePeer",
            "eventDeduplicator.RemovePeer",
            "rateLimiter.Clear()",
            "eventDeduplicator.Clear()"
        })
        {
            StringAssert.Contains(source, marker);
        }

    }

    [TestMethod]
    public void OldRoutedChatDiagnosticsRemainNonEmitting()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");
        var diagnostics = Slice(
            source,
            "private static void ObserveUntrustedRoutedEvent",
            "private static async Task SendGameEventAsync");

        StringAssert.Contains(diagnostics, "RoutedRpcPayload");
        StringAssert.Contains(diagnostics, "did not emit an event");
        Assert.IsFalse(diagnostics.Contains("SendGameEventAsync", StringComparison.Ordinal));
    }

    [TestMethod]
    public void MessageHandlerNegotiatesThenReturnsOnlyAnAuthoritativeAcceptedEvent()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");

        var helloOutput = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));
        var reportOutput = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Chat, 2, new CompanionChatReport(
                "chat-1",
                Now.AddSeconds(2).ToUnixTimeMilliseconds(),
                "hello")),
            Now.AddSeconds(2));

        Assert.IsNull(helloOutput);
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(reportOutput);
        var accepted = (CompanionAcceptedEvent)reportOutput;
        Assert.AreEqual(ValheimEventType.ChatMessage, accepted.Type);
        var json = JsonSerializer.SerializeToElement(accepted.Data);
        Assert.AreEqual("Steam_real", json.GetProperty("player").GetProperty("gameId").GetString());
    }

    [TestMethod]
    public void MessageHandlerRejectsMalformedUnknownAndPreNegotiationReports()
    {
        var harness = CreateHarness();
        var player = Player("Steam_real", "Real Player");

        Assert.IsNull(harness.Handler.Process(PeerId, player, "{", Now));
        Assert.IsNull(harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Chat, 1, new CompanionChatReport(
                "chat-1",
                Now.ToUnixTimeMilliseconds(),
                "hello")),
            Now));

        Begin(harness);
        Assert.IsNull(harness.Handler.Process(
            PeerId + 1,
            player,
            Encode(CompanionMessageTypes.Chat, 1, new CompanionChatReport(
                "chat-2",
                Now.ToUnixTimeMilliseconds(),
                "hello")),
            Now));
    }

    [TestMethod]
    public void MessageHandlerValidatesHeartbeatAndRefreshesNegotiatedSession()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));

        var output = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Heartbeat, 2, new CompanionHeartbeat(
                Now.AddSeconds(2).ToUnixTimeMilliseconds())),
            Now.AddSeconds(2));

        Assert.IsNull(output);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var snapshot));
        Assert.AreEqual(Now.AddSeconds(2), snapshot.LastHeartbeat);
        Assert.AreEqual(2, snapshot.LastSequence);
    }

    [TestMethod]
    public void MessageHandlerUpdatesSharedInventoryWithoutReturningAnEvent()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Inventory)),
            Now.AddSeconds(1));

        var output = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.InventorySnapshot, 2, new CompanionInventoryReport(
                new[]
                {
                    new CompanionInventoryStack("Wood", "Wood", 3, 1, 100, false, 0)
                })),
            Now.AddSeconds(2));

        Assert.IsInstanceOfType<CompanionInventoryUpdated>(output);
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            harness.Inventory.TryGetStable(player.GameId, Now.AddSeconds(3), out var items));
        Assert.AreEqual(3, items.Single().Amount);
    }

    [TestMethod]
    public void MessageHandlerRateLimitsHeartbeatBeforeMutatingSessionSequence()
    {
        var harness = CreateHarness(rateCapacity: 1);
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));
        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Heartbeat, 2, new CompanionHeartbeat(
                Now.AddSeconds(2).ToUnixTimeMilliseconds())),
            Now.AddSeconds(2));

        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Heartbeat, 3, new CompanionHeartbeat(
                Now.AddSeconds(3).ToUnixTimeMilliseconds())),
            Now.AddSeconds(3));

        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var snapshot));
        Assert.AreEqual(2, snapshot.LastSequence);
        Assert.AreEqual(Now.AddSeconds(2), snapshot.LastHeartbeat);
    }

    [TestMethod]
    public void EventQueueIsBoundedFifoAndDropsOldGenerationOnWorldReset()
    {
        var queue = new BoundedCompanionEventQueue(capacity: 2);
        var first = new CompanionAcceptedEvent("first", new { value = 1 });
        var second = new CompanionAcceptedEvent("second", new { value = 2 });

        Assert.IsTrue(queue.TryEnqueue(first));
        Assert.IsTrue(queue.TryEnqueue(second));
        Assert.IsFalse(queue.TryEnqueue(new CompanionAcceptedEvent("overflow", new { value = 3 })));
        Assert.IsTrue(queue.TryDequeue(out var dequeued));
        Assert.AreSame(first, dequeued.Event);
        Assert.AreEqual(0, dequeued.Generation);

        queue.AdvanceGeneration();

        Assert.AreEqual(1, queue.Generation);
        Assert.AreEqual(0, queue.Count);
        Assert.IsFalse(queue.TryDequeue(out _));
        Assert.IsTrue(queue.TryEnqueue(new CompanionAcceptedEvent("current", new { value = 4 })));
        Assert.IsTrue(queue.TryDequeue(out var current));
        Assert.AreEqual(1, current.Generation);
        Assert.AreEqual("current", current.Event.Type);
    }

    [TestMethod]
    public void DisabledModeRegistersNothing()
    {
        var harness = CreateHarness();
        Begin(harness);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));

        var decision = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Disabled,
            session,
            session.ExpiresAt,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: null);
        var bridge = ReadPluginSource("CompanionServerBridge.cs");

        Assert.AreEqual(CompanionEnforcementAction.None, decision.Action);
        Assert.IsFalse(decision.RequiresDisconnect);
        StringAssert.Contains(bridge, "companionMode == CompanionMode.Disabled");
        Assert.IsTrue(
            bridge.IndexOf("companionMode == CompanionMode.Disabled", StringComparison.Ordinal)
            < bridge.IndexOf("ZRoutedRpc.instance", StringComparison.Ordinal));
    }

    [TestMethod]
    public void OptionalModeExpiresCapabilitiesButNeverDisconnects()
    {
        var harness = CreateHarness();
        Begin(harness);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));

        var beforeExpiry = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Optional,
            session,
            session.ExpiresAt - TimeSpan.FromTicks(1),
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: null);
        var decision = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Optional,
            session,
            session.ExpiresAt,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: null);

        Assert.AreEqual(CompanionEnforcementAction.None, beforeExpiry.Action);
        Assert.AreEqual(CompanionEnforcementAction.RestartSession, decision.Action);
        Assert.IsFalse(decision.RequiresDisconnect);
        Assert.AreEqual(CompanionEnforcementReason.None, decision.Reason);
    }

    [TestMethod]
    public void RequiredModeExplainsThenDisconnectsMissingCompanion()
    {
        var harness = CreateHarness();
        Begin(harness);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));

        var beforeExpiry = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Required,
            session,
            session.ExpiresAt - TimeSpan.FromTicks(1),
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: null);
        var decision = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Required,
            session,
            session.ExpiresAt,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: null);
        var bridge = ReadPluginSource("CompanionServerBridge.cs");

        Assert.AreEqual(CompanionEnforcementAction.None, beforeExpiry.Action);
        Assert.AreEqual(CompanionEnforcementAction.ExplainThenDisconnect, decision.Action);
        Assert.AreEqual(CompanionEnforcementReason.MissingCompanion, decision.Reason);
        Assert.IsTrue(decision.RequiresDisconnect);
        StringAssert.Contains(bridge, "ShowMessage");
        StringAssert.Contains(bridge, "peer.m_rpc is not null");
        StringAssert.Contains(bridge, "peer.m_rpc.Invoke(\"Kicked\")");
        StringAssert.Contains(bridge, "network.Disconnect(pending.Peer)");
        StringAssert.Contains(bridge, "DisconnectExplanationGrace");
        StringAssert.Contains(bridge, "DisconnectFallbackGrace");
    }

    [TestMethod]
    public void RequiredModeExplainsExpectedAndActualVersion()
    {
        var harness = CreateHarness();
        Begin(harness);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));

        var decision = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Required,
            session,
            Now.AddSeconds(1),
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: CompanionProtocol.CurrentVersion + 1);

        Assert.AreEqual(CompanionEnforcementAction.ExplainThenDisconnect, decision.Action);
        Assert.AreEqual(CompanionEnforcementReason.IncompatibleProtocol, decision.Reason);
        Assert.AreEqual(CompanionProtocol.MinimumVersion, decision.ExpectedMinimumVersion);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, decision.ExpectedMaximumVersion);
        Assert.AreEqual(CompanionProtocol.CurrentVersion + 1, decision.ActualProtocolVersion);
    }

    [TestMethod]
    public void RequiredModeDisconnectsExpiredHeartbeatAfterGrace()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));

        var beforeExpiry = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Required,
            session,
            session.ExpiresAt - TimeSpan.FromTicks(1),
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: CompanionProtocol.CurrentVersion);
        var decision = CompanionEnforcementPolicy.Evaluate(
            CompanionMode.Required,
            session,
            session.ExpiresAt,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            reportedProtocolVersion: CompanionProtocol.CurrentVersion);

        Assert.AreEqual(CompanionEnforcementAction.None, beforeExpiry.Action);
        Assert.AreEqual(CompanionEnforcementAction.ExplainThenDisconnect, decision.Action);
        Assert.AreEqual(CompanionEnforcementReason.HeartbeatExpired, decision.Reason);
        Assert.IsTrue(decision.RequiresDisconnect);
    }

    [TestMethod]
    public void CompatibleProductPatchDoesNotFailWireCompatibility()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");

        _ = harness.Handler.Process(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.99.123-compatible-patch",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));

        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));
        Assert.IsTrue(session.IsNegotiated);
        Assert.AreEqual("1.99.123-compatible-patch", session.ProductVersion);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, session.SelectedProtocolVersion);
    }

    [TestMethod]
    public void MessageHandlerReportsUnsupportedProtocolWithoutNegotiatingIt()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        var json = $"{{\"protocolVersion\":{CompanionProtocol.CurrentVersion},\"sessionNonce\":\"{Nonce}\",\"sequence\":1,\"messageId\":\"hello-ack-1\",\"type\":\"{CompanionMessageTypes.HelloAck}\",\"payload\":{{\"protocolVersion\":{CompanionProtocol.CurrentVersion + 1},\"productVersion\":\"2.0.0\",\"acceptedCapabilities\":1}}}}";

        var result = harness.Handler.Handle(
            PeerId,
            player,
            json,
            Now.AddSeconds(1));

        Assert.AreEqual(CompanionSessionDecision.RejectVersion, result.SessionDecision);
        Assert.AreEqual(CompanionProtocol.CurrentVersion + 1, result.ReportedProtocolVersion);
        Assert.AreEqual("2.0.0", result.ReportedProductVersion);
        Assert.IsNull(result.Output);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));
        Assert.IsFalse(session.IsNegotiated);
        Assert.AreEqual(0, session.LastSequence);
    }

    [TestMethod]
    public void MessageHandlerRecordsExplicitIncompatibleClientRange()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");

        var result = harness.Handler.Handle(
            PeerId,
            player,
            Encode(
                CompanionMessageTypes.HelloNack,
                1,
                new CompanionHelloNack(2, 3, "3.0.0-client")),
            Now.AddSeconds(1));

        Assert.AreEqual(CompanionSessionDecision.RejectVersion, result.SessionDecision);
        Assert.AreEqual(3, result.ReportedProtocolVersion);
        Assert.AreEqual("3.0.0-client", result.ReportedProductVersion);
        Assert.IsNull(result.Output);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var session));
        Assert.IsFalse(session.IsNegotiated);
    }

    [TestMethod]
    public void RequiredEnforcementRevokesSessionBeforeGraceWindow()
    {
        var harness = CreateHarness();
        Begin(harness);
        var player = Player("Steam_real", "Real Player");
        var unsupportedHelloAck = $"{{\"protocolVersion\":{CompanionProtocol.CurrentVersion},\"sessionNonce\":\"{Nonce}\",\"sequence\":1,\"messageId\":\"hello-ack-unsupported\",\"type\":\"{CompanionMessageTypes.HelloAck}\",\"payload\":{{\"protocolVersion\":{CompanionProtocol.CurrentVersion + 1},\"productVersion\":\"2.0.0\",\"acceptedCapabilities\":1}}}}";

        var rejected = harness.Handler.Handle(
            PeerId,
            player,
            unsupportedHelloAck,
            Now.AddSeconds(1));
        Assert.AreEqual(CompanionSessionDecision.RejectVersion, rejected.SessionDecision);

        harness.Sessions.RemovePeer(PeerId);
        harness.Inventory.RemovePeer(PeerId);
        var compatibleAfterRevocation = harness.Handler.Handle(
            PeerId,
            player,
            Encode(CompanionMessageTypes.HelloAck, 1, new CompanionHelloAck(
                CompanionProtocol.CurrentVersion,
                "1.0.1",
                CompanionCapability.Chat)),
            Now.AddSeconds(1));
        var reportAfterRevocation = harness.Handler.Handle(
            PeerId,
            player,
            Encode(CompanionMessageTypes.Chat, 2, new CompanionChatReport(
                "chat-after-revocation",
                Now.ToUnixTimeMilliseconds(),
                "must not forward")),
            Now.AddSeconds(1));
        var bridge = ReadPluginSource("CompanionServerBridge.cs");
        var expiration = Slice(
            bridge,
            "private void ExpirePeerCapabilities",
            "private void ProcessPendingDisconnects");
        var handler = Slice(
            bridge,
            "private void HandleEnvelopeCore",
            "private bool MatchesTrackedPeer");

        Assert.AreEqual(
            CompanionSessionDecision.RejectUnknownPeer,
            compatibleAfterRevocation.SessionDecision);
        Assert.IsNull(compatibleAfterRevocation.Output);
        Assert.IsNull(reportAfterRevocation.Output);
        Assert.IsFalse(harness.Sessions.TryGetSnapshot(PeerId, out _));
        StringAssert.Contains(expiration, "sessions.RemovePeer(peerId)");
        StringAssert.Contains(handler, "result.SessionDecision == CompanionSessionDecision.RejectVersion");
        StringAssert.Contains(handler, "ExpirePeerCapabilities(sender, tracked)");
    }

    [TestMethod]
    public void RequiredEnforcementDecisionRemainsLatchedUntilDisconnect()
    {
        var bridge = ReadPluginSource("CompanionServerBridge.cs");
        var synchronization = Slice(
            bridge,
            "private void SynchronizeReadyPeers",
            "private void AdvanceRequiredEnforcement");

        StringAssert.Contains(
            synchronization,
            "tracked.EnforcementSchedule is not null");
        StringAssert.Contains(
            synchronization,
            "tracked.EnforcementSchedule.Decision");
        Assert.IsTrue(
            synchronization.IndexOf(
                "tracked.EnforcementSchedule.Decision",
                StringComparison.Ordinal)
            < synchronization.IndexOf(
                "CompanionEnforcementPolicy.Evaluate",
                StringComparison.Ordinal));
    }

    [TestMethod]
    public void EnforcementClockUsesMonotonicElapsedTime()
    {
        var bridge = ReadPluginSource("CompanionServerBridge.cs");

        StringAssert.Contains(bridge, "CreateMonotonicClock()");
        StringAssert.Contains(bridge, "Stopwatch.StartNew()");
        StringAssert.Contains(bridge, "var elapsed = stopwatch.Elapsed");
        StringAssert.Contains(bridge, "startedAt + elapsed");
        Assert.IsFalse(
            bridge.Contains("() => DateTimeOffset.UtcNow", StringComparison.Ordinal));
    }

    [TestMethod]
    public void RequiredDisconnectDeadlineSurvivesRepeatedExplanationFailures()
    {
        var decision = new CompanionEnforcementDecision(
            CompanionEnforcementAction.ExplainThenDisconnect,
            CompanionEnforcementReason.MissingCompanion,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            null);
        var schedule = new CompanionDisconnectSchedule(
            decision,
            Now,
            explanationGrace: TimeSpan.FromSeconds(2),
            fallbackGrace: TimeSpan.FromSeconds(1));

        Assert.AreEqual(CompanionDisconnectStep.Explain, schedule.TakeDueStep(Now));
        schedule.RetryExplanation();
        Assert.AreEqual(
            CompanionDisconnectStep.Explain,
            schedule.TakeDueStep(Now.AddSeconds(1)));
        schedule.RetryExplanation();
        Assert.AreEqual(
            CompanionDisconnectStep.Explain,
            schedule.TakeDueStep(Now.AddSeconds(2) - TimeSpan.FromTicks(1)));
        schedule.RetryExplanation();
        Assert.AreEqual(
            CompanionDisconnectStep.Kick,
            schedule.TakeDueStep(Now.AddSeconds(2)));
    }

    [TestMethod]
    public void RequiredDisconnectScheduleExplainsAndActsOnceUnlessForceRetryIsRequested()
    {
        var decision = new CompanionEnforcementDecision(
            CompanionEnforcementAction.ExplainThenDisconnect,
            CompanionEnforcementReason.MissingCompanion,
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            null);
        var schedule = new CompanionDisconnectSchedule(
            decision,
            Now,
            explanationGrace: TimeSpan.FromSeconds(2),
            fallbackGrace: TimeSpan.FromSeconds(1));

        Assert.AreEqual(CompanionDisconnectStep.Explain, schedule.TakeDueStep(Now));
        Assert.AreEqual(CompanionDisconnectStep.None, schedule.TakeDueStep(Now));
        schedule.RetryExplanation();
        Assert.AreEqual(CompanionDisconnectStep.Explain, schedule.TakeDueStep(Now));
        Assert.AreEqual(
            CompanionDisconnectStep.None,
            schedule.TakeDueStep(Now.AddSeconds(2) - TimeSpan.FromTicks(1)));
        Assert.AreEqual(
            CompanionDisconnectStep.Kick,
            schedule.TakeDueStep(Now.AddSeconds(2)));
        Assert.AreEqual(
            CompanionDisconnectStep.None,
            schedule.TakeDueStep(Now.AddSeconds(2)));
        Assert.AreEqual(
            CompanionDisconnectStep.None,
            schedule.TakeDueStep(Now.AddSeconds(3) - TimeSpan.FromTicks(1)));
        Assert.AreEqual(
            CompanionDisconnectStep.ForceDisconnect,
            schedule.TakeDueStep(Now.AddSeconds(3)));
        schedule.RetryForceDisconnect(Now.AddSeconds(3));
        Assert.AreEqual(
            CompanionDisconnectStep.None,
            schedule.TakeDueStep(Now.AddSeconds(4) - TimeSpan.FromTicks(1)));
        Assert.AreEqual(
            CompanionDisconnectStep.ForceDisconnect,
            schedule.TakeDueStep(Now.AddSeconds(4)));
        Assert.AreEqual(
            CompanionDisconnectStep.None,
            schedule.TakeDueStep(DateTimeOffset.MaxValue));
    }

    [TestMethod]
    public void NegotiationObservationIgnoresReplayAndLatchesFirstVersionRejection()
    {
        var acceptedThenReplay = new CompanionNegotiationObservation();
        acceptedThenReplay.Observe(new CompanionMessageHandlingResult(
            null,
            CompanionSessionDecision.Accept,
            CompanionProtocol.CurrentVersion,
            "1.0.1"));
        acceptedThenReplay.Observe(new CompanionMessageHandlingResult(
            null,
            CompanionSessionDecision.RejectSequence,
            CompanionProtocol.CurrentVersion + 1,
            "2.0.0"));

        Assert.AreEqual(CompanionProtocol.CurrentVersion, acceptedThenReplay.ReportedProtocolVersion);
        Assert.AreEqual("1.0.1", acceptedThenReplay.ReportedProductVersion);
        Assert.IsNull(acceptedThenReplay.RejectedProtocolVersion);

        var rejectedThenCompatible = new CompanionNegotiationObservation();
        rejectedThenCompatible.Observe(new CompanionMessageHandlingResult(
            null,
            CompanionSessionDecision.RejectVersion,
            CompanionProtocol.CurrentVersion + 1,
            "2.0.0"));
        rejectedThenCompatible.Observe(new CompanionMessageHandlingResult(
            null,
            CompanionSessionDecision.Accept,
            CompanionProtocol.CurrentVersion,
            "1.0.2"));

        Assert.AreEqual(CompanionProtocol.CurrentVersion + 1, rejectedThenCompatible.RejectedProtocolVersion);
        Assert.AreEqual(CompanionProtocol.CurrentVersion, rejectedThenCompatible.ReportedProtocolVersion);
        Assert.AreEqual("1.0.2", rejectedThenCompatible.ReportedProductVersion);
    }

    private static Harness CreateHarness(int rateCapacity = 20)
    {
        var sessions = new CompanionSessionRegistry(
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            CompanionCapability.Chat
                | CompanionCapability.Inventory
                | CompanionCapability.PlayerDeath
                | CompanionCapability.EntityKilled,
            TimeSpan.FromSeconds(10),
            TimeSpan.FromSeconds(30));
        var inventory = new CompanionInventoryCache(TimeSpan.FromSeconds(30));
        var rateLimiter = new CompanionRateLimiter(
            rateCapacity,
            1,
            TimeSpan.FromMinutes(1));
        var processor = new CompanionReportProcessor(
            sessions,
            rateLimiter,
            new BoundedEventDeduplicator(128),
            inventory);

        return new Harness(
            sessions,
            inventory,
            new CompanionServerMessageHandler(sessions, rateLimiter, processor));
    }

    private static void Begin(Harness harness)
    {
        harness.Sessions.Begin(PeerId, Now, Nonce);
        harness.Inventory.BeginSession(PeerId, Nonce);
    }

    private static string Encode<T>(string type, long sequence, T payload)
    {
        var envelope = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            Nonce,
            sequence,
            $"message-{sequence}",
            type,
            JsonSerializer.SerializeToElement(payload, WireJson));
        return CompanionEnvelopeCodec.EncodeEnvelope(envelope);
    }

    private static TakaroPlayer Player(string gameId, string name) =>
        new(gameId, name, null, $"valheim:{gameId}", null, null);

    private static string ReadPluginSource(string fileName)
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Plugin",
            fileName));
        Assert.IsTrue(File.Exists(path), $"Missing plugin source: {fileName}");
        return File.ReadAllText(path);
    }

    private static string ReadCoreSource(string fileName)
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Core",
            fileName));
        Assert.IsTrue(File.Exists(path), $"Missing core source: {fileName}");
        return File.ReadAllText(path);
    }

    private static string Slice(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        var end = source.IndexOf(endMarker, StringComparison.Ordinal);
        Assert.IsTrue(start >= 0, $"Missing source marker: {startMarker}");
        Assert.IsTrue(end > start, $"Missing source marker: {endMarker}");
        return source[start..end];
    }

    private sealed record Harness(
        CompanionSessionRegistry Sessions,
        CompanionInventoryCache Inventory,
        CompanionServerMessageHandler Handler);
}
