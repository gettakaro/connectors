using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionServerBridgeContractTests
{
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
    public void ServerBindsReportsToTheRpcSender()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");

        StringAssert.Contains(source, "TryResolveConnectedPeer(sender");
        StringAssert.Contains(source, "messageHandler.Process(sender, player");
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
            < handler.IndexOf("messageHandler.Process(sender, player", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerForwardsOnlyAcceptedEvents()
    {
        var source = ReadPluginSource("CompanionServerBridge.cs");
        var handler = Slice(source, "private void HandleEnvelope", "private void ForwardAcceptedEvent");

        StringAssert.Contains(handler, "output is CompanionAcceptedEvent acceptedEvent");
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
            "CompanionSessionRestartPolicy.ShouldRestart",
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
    public void SessionRestartPolicyRecoversAtTheExactExpiryBoundary()
    {
        var harness = CreateHarness();
        Begin(harness);
        Assert.IsTrue(harness.Sessions.TryGetSnapshot(PeerId, out var snapshot));

        Assert.IsFalse(CompanionSessionRestartPolicy.ShouldRestart(
            snapshot,
            snapshot.ExpiresAt - TimeSpan.FromTicks(1)));
        Assert.IsTrue(CompanionSessionRestartPolicy.ShouldRestart(
            snapshot,
            snapshot.ExpiresAt));
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
