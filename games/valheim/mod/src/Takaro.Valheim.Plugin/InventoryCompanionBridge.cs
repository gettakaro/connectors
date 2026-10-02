using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using System.Diagnostics;
using System.Text.Json;
using HarmonyLib;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Server half of the optional client inventory companion. It offers every ready peer a
/// protocol-3 session; a vanilla client never answers and plays exactly as without it. A
/// companion that answers is bound to the authenticated peer it arrived on and may only feed
/// two things: inventory snapshots (for getPlayerInventory) and kill verdicts (a supplement
/// to the server's own kill detection). Nothing here kicks, waits on, or depends on a client.
/// All work runs on the Unity main thread, bounded and rate-limited; nothing awaits a client.
/// </summary>
public sealed class InventoryCompanionBridge : IDisposable
{
    private static readonly TimeSpan HandshakeGrace = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan HeartbeatGrace = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan FirstHelloRetry = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan MaximumHelloRetry = TimeSpan.FromMinutes(5);
    private static readonly TimeSpan PeerSyncInterval = TimeSpan.FromSeconds(1);
    private const int MaximumLoggedKillVerdictsPerMinute = 30;

    private static readonly JsonSerializerOptions WireJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false
    };

    [ThreadStatic]
    private static ZRpc? arrivalRpc;

    private readonly ValheimPlayerResolver playerResolver;
    private readonly CompanionInventoryCache inventory;
    private readonly CompanionSessionRegistry sessions;
    private readonly CompanionRateLimiter rateLimiter;
    private readonly CompanionRateLimiter killVerdictRateLimiter;
    private readonly CompanionKillVerdictStore killVerdicts;
    private readonly CompanionServerMessageHandler messageHandler;
    private readonly Action<string> log;
    private readonly Func<DateTimeOffset> clock;
    private readonly Dictionary<long, TrackedPeer> trackedPeers = new();
    private ZRoutedRpc? registeredRpc;
    private object? currentWorldIdentity;
    private long? currentWorldUid;
    private bool hasCurrentWorldIdentity;
    private DateTimeOffset nextPeerSyncAt;
    private DateTimeOffset killVerdictLogWindowStart;
    private int killVerdictLogsInWindow;
    private bool disposed;

    public InventoryCompanionBridge(
        ValheimPlayerResolver playerResolver,
        CompanionInventoryCache inventory,
        Action<string>? log = null)
    {
        this.playerResolver = playerResolver ?? throw new ArgumentNullException(nameof(playerResolver));
        this.inventory = inventory ?? throw new ArgumentNullException(nameof(inventory));
        this.log = log ?? (_ => { });
        clock = CreateMonotonicClock();
        sessions = new CompanionSessionRegistry(
            CompanionProtocol.MinimumVersion,
            CompanionProtocol.CurrentVersion,
            CompanionCapability.Inventory,
            HandshakeGrace,
            HeartbeatGrace);
        rateLimiter = new CompanionRateLimiter(capacity: 10, refillTokens: 5, refillInterval: TimeSpan.FromSeconds(1));
        killVerdictRateLimiter = new CompanionRateLimiter(capacity: 8, refillTokens: 4, refillInterval: TimeSpan.FromSeconds(1));
        killVerdicts = new CompanionKillVerdictStore();
        messageHandler = new CompanionServerMessageHandler(
            sessions,
            rateLimiter,
            killVerdictRateLimiter,
            inventory,
            killVerdicts);
        CompanionKillVerdicts.Attach(sessions, killVerdicts, clock);
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
            log($"Takaro Valheim inventory companion bridge update failed: {ex.Message}");
        }
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        CompanionKillVerdicts.Detach(sessions);
        RemoveAllTrackedPeers();
        rateLimiter.Clear();
        killVerdictRateLimiter.Clear();
        killVerdicts.Clear();
        inventory.Clear();
        registeredRpc = null;
    }

    internal static void OnRoutedRpcArrived(ZRpc rpc) => arrivalRpc = rpc;

    internal static void OnRoutedRpcDone() => arrivalRpc = null;

    private void UpdateCore()
    {
        var network = ZNet.instance;
        SwitchWorld(network);

        var routedRpc = ZRoutedRpc.instance;
        if (network is null || routedRpc is null || !network.IsServer())
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
                log($"Takaro Valheim inventory companion RPC registration failed: {ex.Message}");
                return;
            }

            registeredRpc = routedRpc;
            log($"Takaro Valheim inventory companion RPC registered (optional client mod, protocol {CompanionProtocol.CurrentVersion}).");
        }

        var now = clock();
        if (now < nextPeerSyncAt)
        {
            return;
        }

        nextPeerSyncAt = now + PeerSyncInterval;
        SynchronizeReadyPeers(network, routedRpc, now);
    }

    private void SynchronizeReadyPeers(ZNet network, ZRoutedRpc routedRpc, DateTimeOffset now)
    {
        var uniqueReadyPeers = network.GetPeers()
            .Where(peer => peer.IsReady())
            .GroupBy(peer => peer.m_uid)
            .Where(group => group.Take(2).Count() == 1)
            .Select(group => group.First())
            .ToArray();
        var readyPeerIds = new HashSet<long>(uniqueReadyPeers.Select(peer => peer.m_uid));

        foreach (var disconnectedPeerId in trackedPeers.Keys.Where(peerId => !readyPeerIds.Contains(peerId)).ToArray())
        {
            RemovePeer(disconnectedPeerId);
        }

        foreach (var peer in uniqueReadyPeers)
        {
            var characterId = CharacterId(peer);
            if (trackedPeers.TryGetValue(peer.m_uid, out var tracked)
                && (!ReferenceEquals(tracked.Peer, peer)
                    || !string.Equals(tracked.CharacterId, characterId, StringComparison.Ordinal)))
            {
                RemovePeer(peer.m_uid);
            }

            if (!trackedPeers.TryGetValue(peer.m_uid, out tracked))
            {
                tracked = new TrackedPeer(peer, characterId);
                trackedPeers.Add(peer.m_uid, tracked);
                BeginSession(routedRpc, peer, tracked, now);
                continue;
            }

            if (!sessions.TryGetSnapshot(peer.m_uid, out var snapshot))
            {
                BeginSession(routedRpc, peer, tracked, now);
                continue;
            }

            // A version mismatch is latched until the player reconnects: no retries, no kick.
            if (snapshot.IsRejected || now < snapshot.ExpiresAt)
            {
                continue;
            }

            if (snapshot.IsNegotiated)
            {
                log($"Takaro Valheim inventory companion session for peer {peer.m_uid} lapsed (no heartbeat for {HeartbeatGrace.TotalSeconds:0} s); offering a new session.");
                tracked.HelloRetry = FirstHelloRetry;
                BeginSession(routedRpc, peer, tracked, now);
            }
            else if (now >= tracked.NextHelloAt)
            {
                // No answer: a vanilla client, or a companion still loading. Re-offer with
                // backoff so a vanilla client costs at most one small RPC every few minutes.
                BeginSession(routedRpc, peer, tracked, now);
            }
        }
    }

    private void BeginSession(ZRoutedRpc routedRpc, ZNetPeer peer, TrackedPeer tracked, DateTimeOffset now)
    {
        try
        {
            var nonce = Guid.NewGuid().ToString("N");
            sessions.Begin(peer.m_uid, now, nonce);
            inventory.BeginSession(peer.m_uid, nonce);
            var hello = CreateEnvelope(
                CompanionProtocol.NegotiationEnvelopeVersion,
                nonce,
                sequence: 1,
                messageId: "server-hello",
                CompanionMessageTypes.Hello,
                new CompanionHello(
                    CompanionProtocol.MinimumVersion,
                    CompanionProtocol.CurrentVersion,
                    (int)CompanionCapability.Inventory));
            routedRpc.InvokeRoutedRPC(peer.m_uid, CompanionProtocol.RpcName, CompanionEnvelopeCodec.EncodeEnvelope(hello));
            tracked.NextHelloAt = now + tracked.HelloRetry;
            tracked.HelloRetry = tracked.HelloRetry + tracked.HelloRetry > MaximumHelloRetry
                ? MaximumHelloRetry
                : tracked.HelloRetry + tracked.HelloRetry;
            tracked.LoggedSnapshotForNonce = null;
            if (!tracked.LoggedFirstHello)
            {
                tracked.LoggedFirstHello = true;
                log($"Takaro Valheim inventory companion hello sent to peer {peer.m_uid} (protocol {CompanionProtocol.CurrentVersion}; a vanilla client ignores it).");
            }
        }
        catch (Exception ex)
        {
            sessions.RemovePeer(peer.m_uid);
            tracked.NextHelloAt = now + tracked.HelloRetry;
            log($"Takaro Valheim inventory companion session could not start for peer {peer.m_uid}: {ex.Message}");
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
            log($"Takaro Valheim inventory companion envelope handling failed for peer {sender}: {ex.Message}");
        }
    }

    private void HandleEnvelopeCore(long sender, string json)
    {
        var arrival = arrivalRpc;
        if (disposed
            || string.IsNullOrEmpty(json)
            || json.Length > CompanionProtocol.MaximumEnvelopeUtf8Bytes
            || !playerResolver.TryResolveConnectedPeer(sender, out var peer, out var player)
            || peer is null
            || player is null
            || !MatchesTrackedPeer(sender, peer)
            || (arrival is not null && !ReferenceEquals(peer.m_rpc, arrival)))
        {
            return;
        }

        var result = messageHandler.Handle(sender, player, json, clock(), DateTimeOffset.UtcNow);
        var who = $"peer {sender} ({player.Name})";
        switch (result.Outcome)
        {
            case CompanionMessageOutcome.Negotiated:
                log($"Takaro Valheim inventory companion negotiated with {who}: protocol {result.RemoteMaximumVersion}, companion {result.RemoteProductVersion}. Its inventory and kill reports are client-reported and untrusted.");
                break;
            case CompanionMessageOutcome.VersionRejected:
                log($"Takaro Valheim inventory companion on {who} speaks protocol {CompanionVersionPolicy.DescribeRange(result.RemoteMinimumVersion ?? 0, result.RemoteMaximumVersion ?? 0)} (companion {result.RemoteProductVersion}); this server needs protocol {CompanionVersionPolicy.DescribeRange(CompanionProtocol.MinimumVersion, CompanionProtocol.CurrentVersion)}. Ignoring it: the player stays connected and plays normally, but their inventory stays unavailable until they install the matching inventory companion.");
                break;
            case CompanionMessageOutcome.InventoryAccepted:
                if (trackedPeers.TryGetValue(sender, out var tracked)
                    && sessions.TryGetSnapshot(sender, out var session)
                    && !string.Equals(tracked.LoggedSnapshotForNonce, session.Nonce, StringComparison.Ordinal))
                {
                    tracked.LoggedSnapshotForNonce = session.Nonce;
                    log($"Takaro Valheim inventory companion snapshot accepted from {who}: {result.InventoryStacks} stack(s), client-reported.");
                }

                break;
            case CompanionMessageOutcome.KillVerdictAccepted when result.KillVerdict is { } verdict:
                LogKillVerdict(who, verdict);
                break;
        }
    }

    private void LogKillVerdict(string who, CompanionKillVerdict verdict)
    {
        var now = clock();
        if (now - killVerdictLogWindowStart >= TimeSpan.FromMinutes(1))
        {
            killVerdictLogWindowStart = now;
            killVerdictLogsInWindow = 0;
        }

        if (killVerdictLogsInWindow++ < MaximumLoggedKillVerdictsPerMinute)
        {
            log($"Takaro Valheim inventory companion kill verdict from {who}: creature={verdict.CreatureZdo} prefab={verdict.Prefab} enemy={verdict.EnemyToken} lastHitByLocalPlayer={verdict.LastHitByLocalPlayer} lastHitAttackerKind={verdict.LastHitAttackerKind} (client-reported).");
        }
    }

    private bool MatchesTrackedPeer(long sender, ZNetPeer peer) =>
        trackedPeers.TryGetValue(sender, out var tracked)
        && ReferenceEquals(tracked.Peer, peer)
        && string.Equals(tracked.CharacterId, CharacterId(peer), StringComparison.Ordinal);

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
        var worldMarker = new object();
        sessions.SwitchWorld(worldMarker);
        inventory.SwitchWorld(worldMarker);
        rateLimiter.Clear();
        killVerdictRateLimiter.Clear();
        killVerdicts.Clear();
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
        killVerdictRateLimiter.RemovePeer(peerId);
        killVerdicts.RemovePeer(peerId);
        trackedPeers.Remove(peerId);
    }

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

    private static CompanionEnvelope CreateEnvelope<TPayload>(
        int protocolVersion,
        string sessionNonce,
        long sequence,
        string messageId,
        string messageType,
        TPayload payload)
    {
        using var document = JsonDocument.Parse(JsonSerializer.Serialize(payload, WireJsonOptions));
        return new CompanionEnvelope(
            protocolVersion,
            sessionNonce,
            sequence,
            messageId,
            messageType,
            document.RootElement.Clone());
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

        public DateTimeOffset NextHelloAt { get; set; }

        public TimeSpan HelloRetry { get; set; } = FirstHelloRetry;

        public bool LoggedFirstHello { get; set; }

        public string? LoggedSnapshotForNonce { get; set; }
    }
}

// Records which connection a routed RPC physically arrived on, so a companion envelope is
// only accepted from the peer it claims to come from.
[HarmonyPatch(typeof(ZRoutedRpc), "RPC_RoutedRPC")]
internal static class InventoryCompanionArrivalPatch
{
    private static void Prefix(ZRpc rpc)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            InventoryCompanionBridge.OnRoutedRpcArrived(rpc);
        }
    }

    private static void Finalizer() => InventoryCompanionBridge.OnRoutedRpcDone();
}
#else
namespace Takaro.Valheim.Plugin;
#endif
