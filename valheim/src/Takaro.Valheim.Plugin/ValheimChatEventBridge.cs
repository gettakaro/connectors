using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using HarmonyLib;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

internal static class ValheimChatEventBridge
{
    private static readonly int ChatMessageHash = "ChatMessage".GetStableHashCode();
    private static readonly int SayHash = "Say".GetStableHashCode();
    private static readonly int OnDeathHash = "OnDeath".GetStableHashCode();
    private static readonly int DestroyZdoHash = "DestroyZDO".GetStableHashCode();
    private static int routedDiagnosticsRemaining = 40;
    private static ZRoutedRpc? registeredRpc;
    private static TakaroWebSocketRunner? runner;
    private static Action<string> log = _ => { };

    public static void Initialize(TakaroWebSocketRunner? activeRunner, Action<string>? logger)
    {
        runner = activeRunner;
        log = logger ?? (_ => { });
        log($"Takaro Valheim routed RPC diagnostics: ChatMessage={ChatMessageHash}, Say={SayHash}, OnDeath={OnDeathHash}.");
    }

    public static void Shutdown()
    {
        runner = null;
        registeredRpc = null;
    }

    public static void Update()
    {
        var routedRpc = ZRoutedRpc.instance;
        if (routedRpc is null || ReferenceEquals(routedRpc, registeredRpc))
        {
            return;
        }

        registeredRpc = routedRpc;
        log("Takaro Valheim routed diagnostics active.");
    }

    public static void EmitLog(string level, string message)
    {
        if (!ValheimEventAcceptancePolicy.CanEmit(
                ValheimEventType.Log,
                ValheimEventObservationSource.Connector))
        {
            return;
        }

        var activeRunner = runner;
        if (activeRunner is null)
        {
            return;
        }

        _ = SendGameEventAsync(
            activeRunner,
            "log",
            EventFactory.Log(level, message, DateTimeOffset.UtcNow),
            successLog: null,
            failureLogPrefix: "Takaro Valheim log event send failed");
    }

    public static void ObserveRoutedRpc(ZPackage package)
    {
        var originalPosition = package.GetPos();
        try
        {
            var data = new ZRoutedRpc.RoutedRPCData();
            data.Deserialize(package);
            ObserveRoutedRpcData(data);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not inspect routed chat packet: {ex.Message}");
        }
        finally
        {
            package.SetPos(originalPosition);
        }
    }

    public static void ObserveRoutedRpcData(ZRoutedRpc.RoutedRPCData data)
    {
        var originalPosition = data.m_parameters.GetPos();
        try
        {
            data.m_parameters.SetPos(0);
            if (data.m_methodHash == ChatMessageHash)
            {
                ObserveUntrustedRoutedEvent(ValheimEventType.ChatMessage, "ChatMessage", data);
                return;
            }

            if (data.m_methodHash == SayHash)
            {
                ObserveUntrustedRoutedEvent(ValheimEventType.ChatMessage, "Say", data);
                return;
            }

            if (data.m_methodHash == OnDeathHash)
            {
                ObserveUntrustedRoutedEvent(ValheimEventType.PlayerDeath, "OnDeath", data);
                return;
            }

            if (data.m_methodHash == DestroyZdoHash)
            {
                return;
            }

            if (routedDiagnosticsRemaining > 0)
            {
                routedDiagnosticsRemaining--;
                log($"Takaro Valheim observed routed RPC hash={data.m_methodHash}, sender={data.m_senderPeerID}, targetPeer={data.m_targetPeerID}, targetZdo={data.m_targetZDO}.");
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not inspect routed chat data: {ex.Message}");
        }
        finally
        {
            data.m_parameters.SetPos(originalPosition);
        }
    }

    private static void ObserveUntrustedRoutedEvent(
        string eventType,
        string rpcName,
        ZRoutedRpc.RoutedRPCData data)
    {
        if (ValheimEventAcceptancePolicy.CanEmit(
                eventType,
                ValheimEventObservationSource.RoutedRpcPayload))
        {
            return;
        }

        log($"Takaro Valheim observed routed {rpcName} packet but did not emit an event because routed identity and state are not server-owned: sender={data.m_senderPeerID}, targetPeer={data.m_targetPeerID}, targetZdo={data.m_targetZDO}.");
    }

    private static async Task SendGameEventAsync(
        TakaroWebSocketRunner activeRunner,
        string eventType,
        object evt,
        string? successLog,
        string failureLogPrefix)
    {
        try
        {
            await activeRunner.SendGameEventAsync(eventType, evt);
            if (!string.IsNullOrWhiteSpace(successLog))
            {
                log(successLog!);
            }
        }
        catch (Exception ex)
        {
            log($"{failureLogPrefix}: {ex.Message}");
        }
    }

}

[HarmonyPatch(typeof(ZRoutedRpc), "RPC_RoutedRPC")]
internal static class TakaroRoutedRpcPatch
{
    private static void Prefix(ZPackage pkg)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimChatEventBridge.ObserveRoutedRpc(pkg);
        }
    }
}

[HarmonyPatch(typeof(ZRoutedRpc), "RouteRPC")]
internal static class TakaroRouteRpcPatch
{
    private static void Prefix(ZRoutedRpc.RoutedRPCData rpcData)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimChatEventBridge.ObserveRoutedRpcData(rpcData);
        }
    }
}

#else
namespace Takaro.Valheim.Plugin;
#endif
