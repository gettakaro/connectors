using Takaro.Valheim.Companion.Protocol;

#if TAKARO_VALHEIM_COMPANION
using HarmonyLib;
using System.Diagnostics;

namespace Takaro.Valheim.Companion;

internal sealed class CompanionClientBridge : IDisposable
{
    private const CompanionCapability SupportedCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled
        | CompanionCapability.ServerChat;
    private static readonly TimeSpan InventoryPollInterval = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan InventoryRefreshInterval = TimeSpan.FromSeconds(20);

    private readonly Action<string> log;
    private readonly CompanionClientState state = new(
        CompanionProtocol.MinimumVersion,
        CompanionProtocol.CurrentVersion,
        SupportedCapabilities);
    private readonly Stopwatch monotonicClock = Stopwatch.StartNew();
    private readonly CompanionInventoryReader inventoryReader = new();
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

    internal bool TrySendChat(string message)
    {
        if (!initialized
            || disposed
            || string.IsNullOrWhiteSpace(message)
            || message.Length > CompanionProtocol.MaximumChatCharacters)
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
                    CompanionMessageTypes.Chat,
                    new CompanionChatReport(
                        $"chat-{Guid.NewGuid():N}",
                        DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                        message),
                    out var envelope)
                || envelope is null)
            {
                return false;
            }

            return TrySendEnvelope(routedRpc, network, serverPeer, envelope);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Companion could not report local chat: {ex.Message}");
            return false;
        }
    }

    internal bool TrySendPlayerDeath(CompanionPlayerDeathReport report) =>
        report is not null
        && TrySendReport(CompanionMessageTypes.PlayerDeath, report);

    internal bool TrySendEntityKilled(CompanionEntityKilledReport report) =>
        report is not null
        && TrySendReport(CompanionMessageTypes.EntityKilled, report);

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
            log($"Takaro Valheim Companion could not report {messageType}: {ex.Message}");
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
            || envelope is null)
        {
            return;
        }

        if (envelope.Type == CompanionMessageTypes.ServerChat)
        {
            if (!state.TryAcceptServerChat(envelope, out var chat)
                || chat is null
                || Chat.instance is null)
            {
                return;
            }

            Chat.instance.AddString(chat.Sender, chat.Message, Talker.Type.Normal);
            AccessTools.Field(typeof(Chat), "m_hideTimer")?.SetValue(Chat.instance, 0f);
            log($"Takaro Valheim Companion rendered a server message from {chat.Sender} in chat.");
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

        if (!state.ConfirmHelloAckSent(prepared, monotonicClock.Elapsed))
        {
            state.Reset();
            inventoryReader.Reset();
            return;
        }

        inventoryReader.Reset();
        nextInventoryPollAt = monotonicClock.Elapsed;

        log($"Takaro Valheim Companion negotiated protocol {prepared.Envelope.ProtocolVersion} with the connected server.");
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
        log($"Takaro Valheim Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
    }

    public void Update()
    {
        if (!initialized || disposed)
        {
            return;
        }
    }

    internal bool TrySendChat(string message) => false;

    internal bool TrySendPlayerDeath(CompanionPlayerDeathReport report) => false;

    internal bool TrySendEntityKilled(CompanionEntityKilledReport report) => false;

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
