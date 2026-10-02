using Takaro.Valheim.Companion.Protocol;

#if TAKARO_VALHEIM_COMPANION
using System.Diagnostics;

namespace Takaro.Valheim.Companion;

internal sealed class CompanionClientBridge : IDisposable
{
    private const CompanionCapability SupportedCapabilities = CompanionCapability.Inventory;
    private static readonly TimeSpan InventoryPollInterval = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan InventoryRefreshInterval = TimeSpan.FromSeconds(20);

    private readonly Action<string> log;
    private readonly CompanionClientState state = new(
        CompanionProtocol.MinimumVersion,
        CompanionProtocol.CurrentVersion,
        SupportedCapabilities);
    private readonly Stopwatch monotonicClock = Stopwatch.StartNew();
    private readonly CompanionInventoryReader inventoryReader = new();
    private readonly CompanionSendLimiter killVerdictLimiter = new(capacity: 4, refillPerSecond: 2);
    private ZNet? observedNetwork;
    private ZRoutedRpc? registeredRpc;
    private ZNetPeer? activeServerPeer;
    private World? activeWorld;
    private long activeServerUid;
    private long activeWorldUid;
    private TimeSpan nextInventoryPollAt;
    private bool registrationAttempted;
    private bool registrationSucceeded;
    private bool hasActiveContext;
    private bool initialized;
    private bool disposed;

    public CompanionClientBridge(Action<string>? log = null)
    {
        this.log = log ?? (_ => { });
    }

    public void Initialize()
    {
        if (disposed || initialized)
        {
            return;
        }

        initialized = true;
        log($"Takaro Valheim Inventory Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
    }

    public void Update()
    {
        if (!initialized || disposed)
        {
            return;
        }

        try
        {
            UpdateCore();
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Inventory Companion update failed: {ex.Message}");
        }
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        initialized = false;
        ResetConnectionState();
        ClearActiveContext();
        observedNetwork = null;
        registeredRpc = null;
        registrationAttempted = false;
        registrationSucceeded = false;
    }

    private void UpdateCore()
    {
        var network = ZNet.instance;
        var routedRpc = ZRoutedRpc.instance;
        SynchronizeRegistration(network, routedRpc);
        if (network is null
            || routedRpc is null
            || !registrationSucceeded
            || !ReferenceEquals(network, observedNetwork)
            || !ReferenceEquals(routedRpc, registeredRpc)
            || !SynchronizeReadyContext(network, routedRpc, out var serverPeer))
        {
            return;
        }

        if (state.TryCreateHeartbeat(
                monotonicClock.Elapsed,
                DateTimeOffset.UtcNow,
                out var heartbeat)
            && heartbeat is not null)
        {
            _ = TrySendEnvelope(routedRpc, network, serverPeer, heartbeat);
        }

        PollInventory(routedRpc, network, serverPeer);
    }

    /// <summary>
    /// Sends one kill verdict for a creature this client owned, rate-limited. Fire and forget:
    /// the server never answers it, and nothing here waits for the server.
    /// </summary>
    internal bool TrySendKillVerdict(CompanionKillVerdict verdict) =>
        verdict is not null
        && killVerdictLimiter.TryConsume(monotonicClock.Elapsed)
        && TrySendReport(CompanionMessageTypes.KillVerdict, verdict);

    private bool TrySendReport<TPayload>(
        string messageType,
        TPayload payload)
    {
        if (!initialized || disposed)
        {
            return false;
        }

        try
        {
            var network = ZNet.instance;
            var routedRpc = ZRoutedRpc.instance;
            SynchronizeRegistration(network, routedRpc);
            if (network is null
                || routedRpc is null
                || !registrationSucceeded
                || !ReferenceEquals(network, observedNetwork)
                || !ReferenceEquals(routedRpc, registeredRpc)
                || !SynchronizeReadyContext(network, routedRpc, out var serverPeer)
                || !state.TryCreateReport(
                    messageType,
                    payload,
                    out var envelope)
                || envelope is null)
            {
                return false;
            }

            return TrySendEnvelope(routedRpc, network, serverPeer, envelope);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Inventory Companion could not report {messageType}: {ex.Message}");
            return false;
        }
    }

    private void SynchronizeRegistration(
        ZNet? network,
        ZRoutedRpc? routedRpc)
    {
        if (!ReferenceEquals(network, observedNetwork))
        {
            ResetConnectionState();
            ClearActiveContext();
            observedNetwork = network;
        }

        if (!ReferenceEquals(routedRpc, registeredRpc))
        {
            ResetConnectionState();
            ClearActiveContext();
            registeredRpc = routedRpc;
            registrationAttempted = false;
            registrationSucceeded = false;
        }

        if (network is null
            || routedRpc is null
            || registrationAttempted)
        {
            return;
        }

        registrationAttempted = true;
        try
        {
            var sourceRpc = routedRpc;
            sourceRpc.Register<string>(
                CompanionProtocol.RpcName,
                (sender, json) => HandleEnvelope(sourceRpc, sender, json));
            registrationSucceeded = true;
            log("Takaro Valheim Inventory Companion RPC registered.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Inventory Companion RPC registration failed for this routed-RPC instance: {ex.Message}");
        }
    }

    private bool SynchronizeReadyContext(
        ZNet network,
        ZRoutedRpc routedRpc,
        out ZNetPeer serverPeer)
    {
        serverPeer = null!;
        if (!IsConnectionReady(network, routedRpc, out var world, out var readyServerPeer)
            || world is null
            || readyServerPeer is null)
        {
            if (hasActiveContext || state.HasSession)
            {
                ResetConnectionState();
                ClearActiveContext();
            }

            return false;
        }

        var contextChanged = !hasActiveContext
            || !ReferenceEquals(activeWorld, world)
            || !ReferenceEquals(activeServerPeer, readyServerPeer)
            || activeWorldUid != world.m_uid
            || activeServerUid != readyServerPeer.m_uid;
        if (contextChanged)
        {
            ResetConnectionState();
            activeWorld = world;
            activeServerPeer = readyServerPeer;
            activeWorldUid = world.m_uid;
            activeServerUid = readyServerPeer.m_uid;
            hasActiveContext = true;
        }

        serverPeer = readyServerPeer;
        return true;
    }

    private static bool IsConnectionReady(
        ZNet network,
        ZRoutedRpc routedRpc,
        out World? world,
        out ZNetPeer? serverPeer)
    {
        world = null;
        serverPeer = null;
        if (!ReferenceEquals(ZNet.instance, network)
            || !ReferenceEquals(ZRoutedRpc.instance, routedRpc)
            || network.IsServer()
            || ZNet.GetConnectionStatus() != ZNet.ConnectionStatus.Connected)
        {
            return false;
        }

        world = ZNet.World;
        serverPeer = network.GetServerPeer();
        return world is not null
            && serverPeer is not null
            && serverPeer.IsReady()
            && serverPeer.m_uid != 0
            && serverPeer.m_rpc is not null
            && serverPeer.m_rpc.IsConnected();
    }

    private void HandleEnvelope(
        ZRoutedRpc sourceRpc,
        long sender,
        string json)
    {
        try
        {
            HandleEnvelopeCore(sourceRpc, sender, json);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Inventory Companion ignored an invalid server envelope: {ex.Message}");
        }
    }

    private void HandleEnvelopeCore(
        ZRoutedRpc sourceRpc,
        long sender,
        string json)
    {
        var network = ZNet.instance;
        if (disposed
            || !initialized
            || !registrationSucceeded
            || network is null
            || !ReferenceEquals(network, observedNetwork)
            || !ReferenceEquals(sourceRpc, registeredRpc)
            || !ReferenceEquals(ZRoutedRpc.instance, registeredRpc)
            || !SynchronizeReadyContext(network, sourceRpc, out var serverPeer)
            || sender != serverPeer.m_uid
            || !CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _)
            || envelope is null)
        {
            return;
        }

        if (!state.TryPrepareHelloAck(
                envelope,
                TakaroCompanionBuildVersion.ProductVersion,
                out var prepared)
            || prepared is null)
        {
            return;
        }

        var ackJson = CompanionEnvelopeCodec.EncodeEnvelope(prepared.Envelope);
        if (!TrySendJson(sourceRpc, network, serverPeer, ackJson))
        {
            state.CancelHelloAck(prepared);
            return;
        }

        if (prepared.Envelope.Type == CompanionMessageTypes.HelloNack)
        {
            state.Reset();
            inventoryReader.Reset();
            var serverRange = CompanionEnvelopeCodec.TryDecodePayload<CompanionHello>(envelope, out var hello, out _) && hello is not null
                ? CompanionVersionPolicy.DescribeRange(hello.MinimumVersion, hello.MaximumVersion)
                : "unknown";
            log($"Takaro Valheim Inventory Companion: the server speaks companion protocol {serverRange}, this mod speaks {CompanionVersionPolicy.DescribeRange(CompanionProtocol.MinimumVersion, CompanionProtocol.CurrentVersion)}. Inventory reporting stays off; you can keep playing normally. Install the inventory companion that matches the server's Takaro connector.");
            return;
        }

        if (!state.ConfirmHelloAckSent(prepared, monotonicClock.Elapsed))
        {
            state.Reset();
            inventoryReader.Reset();
            return;
        }

        inventoryReader.Reset();
        nextInventoryPollAt = monotonicClock.Elapsed;

        log($"Takaro Valheim Inventory Companion negotiated protocol {prepared.Envelope.ProtocolVersion} with the connected server; reporting this character's inventory to it.");
    }

    private bool TrySendEnvelope(
        ZRoutedRpc routedRpc,
        ZNet network,
        ZNetPeer serverPeer,
        CompanionEnvelope envelope)
    {
        var json = CompanionEnvelopeCodec.EncodeEnvelope(envelope);
        return TrySendJson(routedRpc, network, serverPeer, json);
    }

    private void PollInventory(
        ZRoutedRpc routedRpc,
        ZNet network,
        ZNetPeer serverPeer)
    {
        var now = monotonicClock.Elapsed;
        if (!state.HasCapability(CompanionCapability.Inventory)
            || now < nextInventoryPollAt)
        {
            return;
        }

        nextInventoryPollAt = SaturatingAdd(now, InventoryPollInterval);
        if (!inventoryReader.TryReadChangedOrRefresh(
                Player.m_localPlayer,
                now,
                InventoryRefreshInterval,
                out var snapshot)
            || snapshot is null
            || !state.TryCreateReport(
                CompanionMessageTypes.InventorySnapshot,
                new CompanionInventoryReport(snapshot.Stacks),
                out var envelope)
            || envelope is null
            || !TrySendEnvelope(routedRpc, network, serverPeer, envelope))
        {
            return;
        }

        inventoryReader.MarkSent(snapshot, now);
    }

    private bool TrySendJson(
        ZRoutedRpc routedRpc,
        ZNet network,
        ZNetPeer serverPeer,
        string json)
    {
        if (!hasActiveContext
            || !ReferenceEquals(network, observedNetwork)
            || !ReferenceEquals(routedRpc, registeredRpc)
            || !ReferenceEquals(serverPeer, activeServerPeer)
            || !IsConnectionReady(network, routedRpc, out var world, out var currentServerPeer)
            || world is null
            || currentServerPeer is null
            || !ReferenceEquals(world, activeWorld)
            || !ReferenceEquals(currentServerPeer, serverPeer)
            || world.m_uid != activeWorldUid
            || serverPeer.m_uid != activeServerUid)
        {
            return false;
        }

        try
        {
            routedRpc.InvokeRoutedRPC(serverPeer.m_uid, CompanionProtocol.RpcName, json);
            return true;
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Inventory Companion could not send to the connected server: {ex.Message}");
            return false;
        }
    }

    private void ClearActiveContext()
    {
        activeServerPeer = null;
        activeWorld = null;
        activeServerUid = 0;
        activeWorldUid = 0;
        hasActiveContext = false;
    }

    private void ResetConnectionState()
    {
        state.ResetConnection();
        inventoryReader.Reset();
        nextInventoryPollAt = TimeSpan.Zero;
    }

    private static TimeSpan SaturatingAdd(TimeSpan value, TimeSpan duration) =>
        value > TimeSpan.MaxValue - duration
            ? TimeSpan.MaxValue
            : value + duration;
}
#else
namespace Takaro.Valheim.Companion;

internal sealed class CompanionClientBridge : IDisposable
{
    private readonly Action<string> log;
    private bool initialized;
    private bool disposed;

    public CompanionClientBridge(Action<string>? log = null)
    {
        this.log = log ?? (_ => { });
    }

    public void Initialize()
    {
        if (disposed || initialized)
        {
            return;
        }

        initialized = true;
        log($"Takaro Valheim Inventory Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
    }

    public void Update()
    {
    }

    internal bool TrySendKillVerdict(CompanionKillVerdict verdict) => false;

    public void Dispose()
    {
        disposed = true;
        initialized = false;
    }
}
#endif
