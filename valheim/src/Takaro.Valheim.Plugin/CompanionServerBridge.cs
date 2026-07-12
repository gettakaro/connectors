using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace Takaro.Valheim.Plugin;

public sealed class CompanionServerBridge : IDisposable
{
    private const int MaximumPendingEvents = 256;
    private static readonly TimeSpan DefaultHandshakeGrace = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan DisconnectExplanationGrace = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan DisconnectFallbackGrace = TimeSpan.FromSeconds(1);
    private const CompanionCapability SupportedCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled;

    private static readonly JsonSerializerOptions WireJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false
    };

    private readonly TakaroWebSocketRunner runner;
    private readonly ValheimPlayerResolver playerResolver;
    private readonly CompanionInventoryCache inventory;
    private readonly CompanionSessionRegistry sessions;
    private readonly CompanionRateLimiter rateLimiter;
    private readonly BoundedEventDeduplicator eventDeduplicator;
    private readonly CompanionServerMessageHandler messageHandler;
    private readonly CompanionMode companionMode;
    private readonly Action<string> log;
    private readonly Func<DateTimeOffset> clock;
    private readonly Func<string> nonceFactory;
    private readonly Dictionary<long, TrackedPeer> trackedPeers = new();
    private readonly List<PendingDisconnect> pendingDisconnects = new();
    private readonly BoundedCompanionEventQueue pendingEvents = new(MaximumPendingEvents);
    private CancellationTokenSource? eventForwardingCancellation = new();
    private Task? activeEventSend;
    private ZRoutedRpc? registeredRpc;
    private object? currentWorldIdentity;
    private long? currentWorldUid;
    private bool hasCurrentWorldIdentity;
    private bool disposed;

    public CompanionServerBridge(
        TakaroWebSocketRunner runner,
        ValheimPlayerResolver playerResolver,
        CompanionInventoryCache inventory,
        CompanionMode companionMode,
        Action<string>? log = null)
        : this(
            runner,
            playerResolver,
            inventory,
            new CompanionSessionRegistry(
                CompanionProtocol.MinimumVersion,
                CompanionProtocol.CurrentVersion,
                SupportedCapabilities,
                DefaultHandshakeGrace,
                TimeSpan.FromSeconds(30)),
            new CompanionRateLimiter(
                capacity: 20,
                refillTokens: 20,
                refillInterval: TimeSpan.FromSeconds(1)),
            new BoundedEventDeduplicator(capacity: 4096),
            companionMode,
            log,
            CreateMonotonicClock(),
            () => Guid.NewGuid().ToString("N"))
    {
    }

    internal CompanionServerBridge(
        TakaroWebSocketRunner runner,
        ValheimPlayerResolver playerResolver,
        CompanionInventoryCache inventory,
        CompanionSessionRegistry sessions,
        CompanionRateLimiter rateLimiter,
        BoundedEventDeduplicator eventDeduplicator,
        CompanionMode companionMode,
        Action<string>? log,
        Func<DateTimeOffset> clock,
        Func<string> nonceFactory)
    {
        this.runner = runner ?? throw new ArgumentNullException(nameof(runner));
        this.playerResolver = playerResolver ?? throw new ArgumentNullException(nameof(playerResolver));
        this.inventory = inventory ?? throw new ArgumentNullException(nameof(inventory));
        this.sessions = sessions ?? throw new ArgumentNullException(nameof(sessions));
        this.rateLimiter = rateLimiter ?? throw new ArgumentNullException(nameof(rateLimiter));
        this.eventDeduplicator = eventDeduplicator ?? throw new ArgumentNullException(nameof(eventDeduplicator));
        if (!Enum.IsDefined(typeof(CompanionMode), companionMode))
        {
            throw new ArgumentOutOfRangeException(nameof(companionMode));
        }

        this.companionMode = companionMode;
        this.log = log ?? (_ => { });
        this.clock = clock ?? throw new ArgumentNullException(nameof(clock));
        this.nonceFactory = nonceFactory ?? throw new ArgumentNullException(nameof(nonceFactory));
        messageHandler = new CompanionServerMessageHandler(
            sessions,
            rateLimiter,
            new CompanionReportProcessor(
                sessions,
                rateLimiter,
                eventDeduplicator,
                inventory));
    }

    public void Update()
    {
        if (disposed)
        {
            return;
        }

        try
        {
            UpdateCore();
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim companion bridge update failed: {ex.Message}");
        }
    }

    private void UpdateCore()
    {
        if (companionMode == CompanionMode.Disabled)
        {
            return;
        }

        var network = ZNet.instance;
        SwitchWorld(network);
        PumpEventForwarder();

        var routedRpc = ZRoutedRpc.instance;
        if (network is null || routedRpc is null)
        {
            return;
        }

        if (!ReferenceEquals(routedRpc, registeredRpc))
        {
            RemoveAllTrackedPeers();
            try
            {
                routedRpc.Register<string>(CompanionProtocol.RpcName, HandleEnvelope);
            }
            catch (Exception ex)
            {
                log($"Takaro Valheim companion RPC registration failed: {ex.Message}");
                return;
            }

            registeredRpc = routedRpc;
            log("Takaro Valheim companion RPC registered.");
        }

        SynchronizeReadyPeers(network, routedRpc);
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        ResetEventForwarding(recreate: false);
        RemoveAllTrackedPeers();
        rateLimiter.Clear();
        eventDeduplicator.Clear();
        inventory.Clear();
        pendingDisconnects.Clear();
        registeredRpc = null;
    }

    internal bool TryGetSession(long peerId, out CompanionSessionSnapshot snapshot) =>
        sessions.TryGetSnapshot(peerId, out snapshot);

    private void SynchronizeReadyPeers(ZNet network, ZRoutedRpc routedRpc)
    {
        var now = clock();
        ProcessPendingDisconnects(network, now);
        var uniqueReadyPeers = network.GetPeers()
            .Where(peer => peer.IsReady())
            .GroupBy(peer => peer.m_uid)
            .Where(group => group.Take(2).Count() == 1)
            .Select(group => group.First())
            .ToArray();
        var readyPeerIds = uniqueReadyPeers
            .Select(peer => peer.m_uid)
            .ToHashSet();

        foreach (var disconnectedPeerId in trackedPeers
                     .Keys
                     .Where(peerId => !readyPeerIds.Contains(peerId))
                     .ToArray())
        {
            RemovePeer(disconnectedPeerId);
        }

        foreach (var peer in uniqueReadyPeers)
        {
            var characterId = peer.m_characterID.IsNone()
                ? null
                : peer.m_characterID.ToString();
            if (trackedPeers.TryGetValue(peer.m_uid, out var tracked)
                && (!ReferenceEquals(tracked.Peer, peer)
                    || !string.Equals(
                        tracked.CharacterId,
                        characterId,
                        StringComparison.Ordinal)))
            {
                RemovePeer(peer.m_uid);
            }

            if (!trackedPeers.ContainsKey(peer.m_uid))
            {
                trackedPeers.Add(peer.m_uid, new TrackedPeer(peer, characterId));
                BeginSession(routedRpc, peer);
            }

            if (!trackedPeers.TryGetValue(peer.m_uid, out tracked))
            {
                continue;
            }

            CompanionEnforcementDecision decision;
            if (tracked.EnforcementSchedule is not null)
            {
                decision = tracked.EnforcementSchedule.Decision;
            }
            else
            {
                sessions.TryGetSnapshot(peer.m_uid, out var snapshot);
                decision = CompanionEnforcementPolicy.Evaluate(
                    companionMode,
                    snapshot,
                    now,
                    CompanionProtocol.MinimumVersion,
                    CompanionProtocol.CurrentVersion,
                    tracked.Negotiation.RejectedProtocolVersion
                        ?? tracked.Negotiation.ReportedProtocolVersion);
            }

            switch (decision.Action)
            {
                case CompanionEnforcementAction.None:
                    break;
                case CompanionEnforcementAction.RestartSession:
                    RemovePeer(peer.m_uid);
                    trackedPeers.Add(peer.m_uid, new TrackedPeer(peer, characterId));
                    BeginSession(routedRpc, peer);
                    break;
                case CompanionEnforcementAction.ExplainThenDisconnect:
                    ExpirePeerCapabilities(peer.m_uid, tracked);
                    AdvanceRequiredEnforcement(
                        routedRpc,
                        peer,
                        tracked,
                        decision,
                        now);
                    break;
                default:
                    throw new InvalidOperationException(
                        $"Unknown companion enforcement action {decision.Action}.");
            }
        }
    }

    private void AdvanceRequiredEnforcement(
        ZRoutedRpc routedRpc,
        ZNetPeer peer,
        TrackedPeer tracked,
        CompanionEnforcementDecision decision,
        DateTimeOffset now)
    {
        tracked.EnforcementSchedule ??= new CompanionDisconnectSchedule(
            decision,
            now,
            DisconnectExplanationGrace,
            DisconnectFallbackGrace);
        if (tracked.EnforcementTransferred)
        {
            return;
        }

        switch (tracked.EnforcementSchedule.TakeDueStep(now))
        {
            case CompanionDisconnectStep.Explain:
                if (!SendRequiredCompanionExplanation(
                        routedRpc,
                        peer,
                        RequiredCompanionExplanation(decision)))
                {
                    tracked.EnforcementSchedule.RetryExplanation();
                    return;
                }

                log($"Takaro Valheim required companion enforcement scheduled for peer {peer.m_uid}: reason={decision.Reason}, expected={ExpectedProtocolRange(decision)}, actual={decision.ActualProtocolVersion?.ToString() ?? "missing"}.");
                break;
            case CompanionDisconnectStep.Kick:
                var kickedRpcSent = false;
                try
                {
                    if (peer.m_rpc is not null)
                    {
                        peer.m_rpc.Invoke("Kicked");
                        kickedRpcSent = true;
                    }
                }
                catch (Exception ex)
                {
                    log($"Takaro Valheim could not send the built-in kicked RPC to peer {peer.m_uid}; the exact-peer disconnect fallback remains scheduled: {ex.Message}");
                }

                tracked.EnforcementTransferred = true;
                pendingDisconnects.Add(new PendingDisconnect(
                    peer,
                    tracked.CharacterId,
                    tracked.EnforcementSchedule));
                log(kickedRpcSent
                    ? $"Takaro Valheim sent the built-in kicked RPC to peer {peer.m_uid} after the companion explanation grace period."
                    : $"Takaro Valheim scheduled the exact-peer disconnect fallback for peer {peer.m_uid} because the built-in kicked RPC was unavailable.");
                break;
            case CompanionDisconnectStep.None:
                break;
            default:
                throw new InvalidOperationException(
                    "Companion enforcement reached an invalid step before transfer to the disconnect fallback.");
        }
    }

    private void ExpirePeerCapabilities(long peerId, TrackedPeer tracked)
    {
        sessions.RemovePeer(peerId);
        if (tracked.CapabilitiesExpired)
        {
            return;
        }

        inventory.RemovePeer(peerId);
        rateLimiter.RemovePeer(peerId);
        eventDeduplicator.RemovePeer(peerId);
        tracked.CapabilitiesExpired = true;
    }

    private void ProcessPendingDisconnects(ZNet network, DateTimeOffset now)
    {
        if (pendingDisconnects.Count == 0)
        {
            return;
        }

        var connectedPeers = network.GetPeers().ToArray();
        foreach (var pending in pendingDisconnects.ToArray())
        {
            var isSameConnection = connectedPeers.Any(peer => ReferenceEquals(peer, pending.Peer))
                && string.Equals(
                    pending.CharacterId,
                    CharacterId(pending.Peer),
                    StringComparison.Ordinal);
            if (!isSameConnection)
            {
                pendingDisconnects.Remove(pending);
                continue;
            }

            if (pending.Schedule.TakeDueStep(now)
                == CompanionDisconnectStep.ForceDisconnect)
            {
                try
                {
                    network.Disconnect(pending.Peer);
                    log($"Takaro Valheim disconnected exact peer {pending.Peer.m_uid} after the built-in kicked RPC fallback grace period.");
                    pendingDisconnects.Remove(pending);
                }
                catch (Exception ex)
                {
                    pending.Schedule.RetryForceDisconnect(now);
                    log($"Takaro Valheim exact-peer disconnect fallback failed for peer {pending.Peer.m_uid} and will retry after the fallback grace period: {ex.Message}");
                }
            }
        }
    }

    private bool SendRequiredCompanionExplanation(
        ZRoutedRpc routedRpc,
        ZNetPeer peer,
        string message)
    {
        var sent = false;
        try
        {
            routedRpc.InvokeRoutedRPC(
                peer.m_uid,
                "ShowMessage",
                (int)MessageHud.MessageType.Center,
                message);
            sent = true;
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not send the center companion explanation to peer {peer.m_uid}: {ex.Message}");
        }

        try
        {
            routedRpc.InvokeRoutedRPC(
                peer.m_uid,
                "ShowMessage",
                (int)MessageHud.MessageType.TopLeft,
                message);
            sent = true;
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not send the top-left companion explanation to peer {peer.m_uid}: {ex.Message}");
        }

        if (!peer.m_characterID.IsNone())
        {
            try
            {
                routedRpc.InvokeRoutedRPC(
                    peer.m_uid,
                    peer.m_characterID,
                    "Message",
                    (int)MessageHud.MessageType.Center,
                    message,
                    0);
                sent = true;
            }
            catch (Exception ex)
            {
                log($"Takaro Valheim could not send the character companion explanation to peer {peer.m_uid}: {ex.Message}");
            }
        }

        return sent;
    }

    private static string RequiredCompanionExplanation(
        CompanionEnforcementDecision decision)
    {
        var expected = ExpectedProtocolRange(decision);
        switch (decision.Reason)
        {
            case CompanionEnforcementReason.IncompatibleProtocol:
                return $"Takaro Valheim Companion is incompatible. This server expects protocol {expected}; your client reported {decision.ActualProtocolVersion?.ToString() ?? "unknown"}. Install or update the Takaro Valheim Companion, then reconnect.";
            case CompanionEnforcementReason.HeartbeatExpired:
                return $"Takaro Valheim Companion stopped responding. This server requires protocol {expected}. Restart or update the companion, then reconnect.";
            case CompanionEnforcementReason.MissingCompanion:
            default:
                return $"Takaro Valheim Companion is required. This server expects protocol {expected}. Install or enable the companion, then reconnect.";
        }
    }

    private static string ExpectedProtocolRange(
        CompanionEnforcementDecision decision) =>
        decision.ExpectedMinimumVersion == decision.ExpectedMaximumVersion
            ? decision.ExpectedMinimumVersion.ToString()
            : $"{decision.ExpectedMinimumVersion}-{decision.ExpectedMaximumVersion}";

    private static string? CharacterId(ZNetPeer peer) =>
        peer.m_characterID.IsNone() ? null : peer.m_characterID.ToString();

    private static Func<DateTimeOffset> CreateMonotonicClock()
    {
        var startedAt = DateTimeOffset.UtcNow;
        var stopwatch = Stopwatch.StartNew();
        return () =>
        {
            var elapsed = stopwatch.Elapsed;
            return startedAt > DateTimeOffset.MaxValue - elapsed
                ? DateTimeOffset.MaxValue
                : startedAt + elapsed;
        };
    }

    private void BeginSession(ZRoutedRpc routedRpc, ZNetPeer peer)
    {
        try
        {
            var now = clock();
            var nonce = nonceFactory();
            var session = sessions.Begin(peer.m_uid, now, nonce);
            inventory.BeginSession(peer.m_uid, session.Nonce);
            var hello = CreateEnvelope(
                CompanionVersionPolicy.SelectNegotiationEnvelopeVersion(
                    CompanionProtocol.MinimumVersion,
                    CompanionProtocol.CurrentVersion),
                session.Nonce,
                sequence: 1,
                messageId: "server-hello",
                CompanionMessageTypes.Hello,
                new CompanionHello(
                    CompanionProtocol.MinimumVersion,
                    CompanionProtocol.CurrentVersion,
                    SupportedCapabilities));
            routedRpc.InvokeRoutedRPC(
                peer.m_uid,
                CompanionProtocol.RpcName,
                CompanionEnvelopeCodec.EncodeEnvelope(hello));
            log($"Takaro Valheim companion hello sent to peer {peer.m_uid}.");
        }
        catch (Exception ex)
        {
            RemovePeer(peer.m_uid);
            log($"Takaro Valheim companion session could not start for peer {peer.m_uid}: {ex.Message}");
        }
    }

    private void HandleEnvelope(long sender, string json)
    {
        try
        {
            HandleEnvelopeCore(sender, json);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim companion envelope handling failed for peer {sender}: {ex.Message}");
        }
    }

    private void HandleEnvelopeCore(long sender, string json)
    {
        if (disposed
            || string.IsNullOrEmpty(json)
            || json.Length > CompanionProtocol.MaximumEnvelopeUtf8Bytes
            || Encoding.UTF8.GetByteCount(json) > CompanionProtocol.MaximumEnvelopeUtf8Bytes
            || !playerResolver.TryResolveConnectedPeer(sender, out var peer, out var player)
            || peer is null
            || player is null
            || !MatchesTrackedPeer(sender, peer))
        {
            return;
        }

        var result = messageHandler.Handle(sender, player, json, clock());
        if (trackedPeers.TryGetValue(sender, out var tracked))
        {
            tracked.Negotiation.Observe(result);
            if (companionMode == CompanionMode.Required
                && result.SessionDecision == CompanionSessionDecision.RejectVersion)
            {
                ExpirePeerCapabilities(sender, tracked);
            }
        }

        if (result.Output is CompanionAcceptedEvent acceptedEvent)
        {
            ForwardAcceptedEvent(acceptedEvent);
        }
    }

    private bool MatchesTrackedPeer(long sender, ZNetPeer peer)
    {
        var characterId = peer.m_characterID.IsNone()
            ? null
            : peer.m_characterID.ToString();
        return trackedPeers.TryGetValue(sender, out var tracked)
            && ReferenceEquals(tracked.Peer, peer)
            && string.Equals(
                tracked.CharacterId,
                characterId,
                StringComparison.Ordinal);
    }

    private void ForwardAcceptedEvent(CompanionAcceptedEvent acceptedEvent) =>
        EnqueueAcceptedEvent(acceptedEvent);

    private void EnqueueAcceptedEvent(CompanionAcceptedEvent acceptedEvent)
    {
        if (!pendingEvents.TryEnqueue(acceptedEvent))
        {
            log($"Takaro Valheim companion event queue is full; dropping accepted {acceptedEvent.Type} event.");
        }
    }

    private void PumpEventForwarder()
    {
        if (activeEventSend is { IsCompleted: false }
            || eventForwardingCancellation is null)
        {
            return;
        }

        activeEventSend = null;
        while (pendingEvents.TryDequeue(out var queuedEvent))
        {
            if (queuedEvent.Generation != pendingEvents.Generation)
            {
                continue;
            }

            activeEventSend = SendQueuedEventAsync(
                queuedEvent.Event,
                eventForwardingCancellation.Token);
            return;
        }
    }

    private async Task SendQueuedEventAsync(
        CompanionAcceptedEvent acceptedEvent,
        CancellationToken cancellationToken)
    {
        try
        {
            await runner.SendGameEventAsync(
                acceptedEvent.Type,
                acceptedEvent.Data,
                cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim companion event send failed for {acceptedEvent.Type}: {ex.Message}");
        }
    }

    private void SwitchWorld(ZNet? worldIdentity)
    {
        long? worldUid = worldIdentity is null ? null : worldIdentity.GetWorldUID();
        if (hasCurrentWorldIdentity
            && ReferenceEquals(currentWorldIdentity, worldIdentity)
            && currentWorldUid == worldUid)
        {
            return;
        }

        RemoveAllTrackedPeers();
        ResetEventForwarding(recreate: true);
        pendingDisconnects.Clear();
        var worldMarker = new object();
        sessions.SwitchWorld(worldMarker);
        inventory.SwitchWorld(worldMarker);
        rateLimiter.Clear();
        eventDeduplicator.Clear();
        currentWorldIdentity = worldIdentity;
        currentWorldUid = worldUid;
        hasCurrentWorldIdentity = true;
    }

    private void RemoveAllTrackedPeers()
    {
        foreach (var peerId in trackedPeers.Keys.ToArray())
        {
            RemovePeer(peerId);
        }
    }

    private void RemovePeer(long peerId)
    {
        sessions.RemovePeer(peerId);
        inventory.RemovePeer(peerId);
        rateLimiter.RemovePeer(peerId);
        eventDeduplicator.RemovePeer(peerId);
        trackedPeers.Remove(peerId);
    }

    private void ResetEventForwarding(bool recreate)
    {
        pendingEvents.AdvanceGeneration();
        var retiringCancellation = eventForwardingCancellation;
        var retiringSend = activeEventSend;
        eventForwardingCancellation = recreate ? new CancellationTokenSource() : null;
        activeEventSend = null;
        if (retiringCancellation is null)
        {
            return;
        }

        retiringCancellation.Cancel();
        if (retiringSend is null || retiringSend.IsCompleted)
        {
            retiringCancellation.Dispose();
            return;
        }

        _ = retiringSend.ContinueWith(
            (_, state) => ((CancellationTokenSource)state!).Dispose(),
            retiringCancellation,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private sealed class TrackedPeer
    {
        public TrackedPeer(ZNetPeer peer, string? characterId)
        {
            Peer = peer;
            CharacterId = characterId;
        }

        public ZNetPeer Peer { get; }

        public string? CharacterId { get; }

        public CompanionNegotiationObservation Negotiation { get; } = new();

        public CompanionDisconnectSchedule? EnforcementSchedule { get; set; }

        public bool EnforcementTransferred { get; set; }

        public bool CapabilitiesExpired { get; set; }
    }

    private sealed class PendingDisconnect
    {
        public PendingDisconnect(
            ZNetPeer peer,
            string? characterId,
            CompanionDisconnectSchedule schedule)
        {
            Peer = peer;
            CharacterId = characterId;
            Schedule = schedule;
        }

        public ZNetPeer Peer { get; }

        public string? CharacterId { get; }

        public CompanionDisconnectSchedule Schedule { get; }
    }

    private static CompanionEnvelope CreateEnvelope<TPayload>(
        int protocolVersion,
        string sessionNonce,
        long sequence,
        string messageId,
        string messageType,
        TPayload payload)
    {
        using var document = JsonDocument.Parse(
            JsonSerializer.Serialize(payload, WireJsonOptions));
        return new CompanionEnvelope(
            protocolVersion,
            sessionNonce,
            sequence,
            messageId,
            messageType,
            document.RootElement.Clone());
    }
}
#else
namespace Takaro.Valheim.Plugin;
#endif
