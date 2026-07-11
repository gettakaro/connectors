using Takaro.Valheim.Companion.Protocol;

#if TAKARO_VALHEIM_COMPANION
using System.Diagnostics;

namespace Takaro.Valheim.Companion;

internal sealed class CompanionClientBridge : IDisposable
{
    private const CompanionCapability SupportedCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled;

    private readonly Action<string> log;
    private readonly CompanionClientState state = new(
        CompanionProtocol.MinimumVersion,
        CompanionProtocol.CurrentVersion,
        SupportedCapabilities);
    private readonly Stopwatch monotonicClock = Stopwatch.StartNew();
    private ZNet? observedNetwork;
    private ZRoutedRpc? registeredRpc;
    private ZNetPeer? activeServerPeer;
    private World? activeWorld;
    private long activeServerUid;
    private long activeWorldUid;
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
        log($"Takaro Valheim Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
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
            log($"Takaro Valheim Companion update failed: {ex.Message}");
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
        state.ResetConnection();
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
            SendEnvelope(routedRpc, network, serverPeer, heartbeat);
        }
    }

    private void SynchronizeRegistration(
        ZNet? network,
        ZRoutedRpc? routedRpc)
    {
        if (!ReferenceEquals(network, observedNetwork))
        {
            state.ResetConnection();
            ClearActiveContext();
            observedNetwork = network;
        }

        if (!ReferenceEquals(routedRpc, registeredRpc))
        {
            state.ResetConnection();
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
            log("Takaro Valheim Companion RPC registered.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Companion RPC registration failed for this routed-RPC instance: {ex.Message}");
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
                state.ResetConnection();
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
            state.ResetConnection();
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
            log($"Takaro Valheim Companion ignored an invalid server envelope: {ex.Message}");
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
            || envelope is null
            || !state.TryPrepareHelloAck(
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

        if (!state.ConfirmHelloAckSent(prepared, monotonicClock.Elapsed))
        {
            state.Reset();
            return;
        }

        log($"Takaro Valheim Companion negotiated protocol {prepared.Envelope.ProtocolVersion} with the connected server.");
    }

    private void SendEnvelope(
        ZRoutedRpc routedRpc,
        ZNet network,
        ZNetPeer serverPeer,
        CompanionEnvelope envelope)
    {
        var json = CompanionEnvelopeCodec.EncodeEnvelope(envelope);
        _ = TrySendJson(routedRpc, network, serverPeer, json);
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
            log($"Takaro Valheim Companion could not send to the connected server: {ex.Message}");
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
        log($"Takaro Valheim Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
    }

    public void Update()
    {
        if (!initialized || disposed)
        {
            return;
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
    }
}
#endif
