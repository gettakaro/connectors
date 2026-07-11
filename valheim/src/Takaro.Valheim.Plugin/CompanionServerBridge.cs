using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using System.Text;
using System.Text.Json;

namespace Takaro.Valheim.Plugin;

public sealed class CompanionServerBridge : IDisposable
{
    private const int MaximumPendingEvents = 256;
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
    private readonly Action<string> log;
    private readonly Func<DateTimeOffset> clock;
    private readonly Func<string> nonceFactory;
    private readonly Dictionary<long, TrackedPeer> trackedPeers = new();
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
        Action<string>? log = null)
        : this(
            runner,
            playerResolver,
            inventory,
            new CompanionSessionRegistry(
                CompanionProtocol.MinimumVersion,
                CompanionProtocol.CurrentVersion,
                SupportedCapabilities,
                TimeSpan.FromSeconds(10),
                TimeSpan.FromSeconds(30)),
            new CompanionRateLimiter(
                capacity: 20,
                refillTokens: 20,
                refillInterval: TimeSpan.FromSeconds(1)),
            new BoundedEventDeduplicator(capacity: 4096),
            log,
            () => DateTimeOffset.UtcNow,
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
        registeredRpc = null;
    }

    internal bool TryGetSession(long peerId, out CompanionSessionSnapshot snapshot) =>
        sessions.TryGetSnapshot(peerId, out snapshot);

    private void SynchronizeReadyPeers(ZNet network, ZRoutedRpc routedRpc)
    {
        var now = clock();
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
                        StringComparison.Ordinal)
                    || !sessions.TryGetSnapshot(peer.m_uid, out var snapshot)
                    || CompanionSessionRestartPolicy.ShouldRestart(snapshot, now)))
            {
                RemovePeer(peer.m_uid);
            }

            if (!trackedPeers.ContainsKey(peer.m_uid))
            {
                trackedPeers.Add(peer.m_uid, new TrackedPeer(peer, characterId));
                BeginSession(routedRpc, peer);
            }
        }
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

        var output = messageHandler.Process(sender, player, json, clock());
        if (output is CompanionAcceptedEvent acceptedEvent)
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
    }

    private static CompanionEnvelope CreateEnvelope<TPayload>(
        string sessionNonce,
        long sequence,
        string messageId,
        string messageType,
        TPayload payload)
    {
        using var document = JsonDocument.Parse(
            JsonSerializer.Serialize(payload, WireJsonOptions));
        return new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
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
