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
    private static readonly HashSet<int> UndecodedDedicatedServerChatHashes = [199378019];
    private static readonly object Sync = new();
    private static readonly Dictionary<string, DateTimeOffset> RecentEntityDeaths = new(StringComparer.Ordinal);
    private static readonly System.Reflection.FieldInfo? LastHitField = AccessTools.Field(typeof(Character), "m_lastHit");
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
        lock (Sync)
        {
            RecentEntityDeaths.Clear();
        }
    }

    public static void Update()
    {
        var routedRpc = ZRoutedRpc.instance;
        if (routedRpc is null || ReferenceEquals(routedRpc, registeredRpc))
        {
            return;
        }

        registeredRpc = routedRpc;
        log("Takaro Valheim routed diagnostics and server entity bridge active.");
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

            if (UndecodedDedicatedServerChatHashes.Contains(data.m_methodHash))
            {
                LogUndecodedDedicatedChatPacket(data);
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

    private static void LogUndecodedDedicatedChatPacket(ZRoutedRpc.RoutedRPCData data)
    {
        if (routedDiagnosticsRemaining <= 0)
        {
            return;
        }

        routedDiagnosticsRemaining--;
        log($"Takaro Valheim observed undecoded dedicated-server chat candidate hash={data.m_methodHash}, sender={data.m_senderPeerID}, targetPeer={data.m_targetPeerID}, targetZdo={data.m_targetZDO}; not emitting until payload layout is known.");
        TryLogDedicatedChatCandidate(data, "int+UserInfo+string", package =>
        {
            var chatType = package.ReadInt();
            var userInfo = new UserInfo();
            userInfo.Deserialize(ref package);
            var text = package.ReadString();
            return $"type={chatType}, user='{SafeForLog(userInfo.Name)}'/'{SafeForLog(userInfo.GetDisplayName())}', text='{SafeForLog(text)}', safe={IsSafeChatText(text)}";
        });
        TryLogDedicatedChatCandidate(data, "string+string", package =>
        {
            var first = package.ReadString();
            var second = package.ReadString();
            return $"first='{SafeForLog(first)}', second='{SafeForLog(second)}', secondSafe={IsSafeChatText(second)}";
        });
    }

    private static void TryLogDedicatedChatCandidate(ZRoutedRpc.RoutedRPCData data, string shape, Func<ZPackage, string> reader)
    {
        var originalPosition = data.m_parameters.GetPos();
        try
        {
            data.m_parameters.SetPos(0);
            log($"Takaro Valheim dedicated chat candidate {shape}: {reader(data.m_parameters)}.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim dedicated chat candidate {shape} failed: {ex.Message}.");
        }
        finally
        {
            data.m_parameters.SetPos(originalPosition);
        }
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

    internal static void EmitEntityKilled(Character character)
    {
        if (!ValheimEventAcceptancePolicy.CanEmit(
                ValheimEventType.EntityKilled,
                ValheimEventObservationSource.ServerCharacterState)
            || !IsDedicatedServer()
            || character is Player
            || character.GetComponent<Player>() != null
            || !ShouldEmitEntityDeath(character))
        {
            return;
        }

        var activeRunner = runner;
        if (activeRunner is null)
        {
            return;
        }

        try
        {
            var position = character.transform.position;
            var killer = TryMapCharacterToTakaroPlayer(GetLastHit(character)?.GetAttacker(), out var player)
                ? player
                : null;
            var hit = GetLastHit(character);
            var entity = new TakaroEntity(
                string.IsNullOrWhiteSpace(character.name) ? character.GetHoverName() : character.name,
                DisplayName(character.m_name, character.GetHoverName()));
            var evt = EventFactory.EntityKilled(
                entity,
                DateTimeOffset.UtcNow,
                new TakaroPosition(position.x, position.y, position.z, "valheim"),
                killer,
                hit is null ? null : hit.m_skill.ToString());

            _ = SendGameEventAsync(
                activeRunner,
                "entity-killed",
                evt,
                $"Takaro Valheim entity-killed event sent for {entity.Code}.",
                "Takaro Valheim entity-killed event send failed");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim entity death emit failed: {ex.Message}");
            EmitLog("error", $"Entity death emit failed: {ex.Message}");
        }
    }

    private static TakaroPlayer? FindTakaroPlayer(string? identifier)
    {
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return null;
        }

        var players = ZNet.instance?.GetPlayerList()
            .Select(ToTakaroPlayer)
            .ToArray() ?? [];

        return PlayerMapper.Find(players, identifier!);
    }

    private static bool TryMapCharacterToTakaroPlayer(Character? character, out TakaroPlayer? player)
    {
        if (character is null)
        {
            player = null;
            return false;
        }

        var zdoId = character.GetZDOID();
        foreach (var info in ZNet.instance?.GetPlayerList() ?? [])
        {
            if (info.m_characterID.Equals(zdoId)
                || Matches(info.m_name, character.GetHoverName())
                || Matches(info.m_serverAssignedDisplayName, character.GetHoverName()))
            {
                player = ToTakaroPlayer(info);
                return true;
            }
        }

        player = FindTakaroPlayer(character.GetHoverName());
        return player is not null;
    }

    private static TakaroPlayer ToTakaroPlayer(ZNet.PlayerInfo player)
    {
        var playerId = FirstNonEmpty(player.m_userInfo.m_id.ToString(), player.m_characterID.ToString());
        return PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
            FirstNonEmpty(player.m_name, player.m_serverAssignedDisplayName, player.m_userInfo.m_displayName, playerId),
            playerId,
            null,
            null,
            null));
    }

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

    private static bool IsSafeChatText(string? value) =>
        !string.IsNullOrWhiteSpace(value)
        && value!.Length <= 512
        && !value.Any(IsUnsafeChatCharacter);

    private static bool IsUnsafeChatCharacter(char value) =>
        value == '\0' || value == '\uFFFD' || (char.IsControl(value) && value is not '\t' and not '\r' and not '\n');

    private static string SafeForLog(string? value)
    {
        if (value is null)
        {
            return "<null>";
        }

        var sanitized = new string(value.Select(ch => IsUnsafeChatCharacter(ch) ? '?' : ch).Take(80).ToArray());
        return value.Length > sanitized.Length ? sanitized + "…" : sanitized;
    }

    private static bool Matches(string? value, string? needle) =>
        !string.IsNullOrWhiteSpace(value)
        && !string.IsNullOrWhiteSpace(needle)
        && value!.Equals(needle, StringComparison.OrdinalIgnoreCase);

    private static string DisplayName(string? rawName, string fallback)
    {
        if (string.IsNullOrWhiteSpace(rawName))
        {
            return fallback;
        }

        var displayName = rawName!.Trim().Trim('$');
        return string.IsNullOrWhiteSpace(displayName) ? fallback : displayName;
    }

    private static bool IsDedicatedServer() =>
        ZNet.instance is not null && ZNet.instance.IsDedicated();

    private static bool ShouldEmitEntityDeath(Character character)
    {
        var position = character.transform.position;
        var zdoId = character.GetZDOID();
        var key = zdoId.IsNone()
            ? $"{character.name}|{position.x:F1}|{position.y:F1}|{position.z:F1}"
            : zdoId.ToString();

        return ShouldEmitEntityDeath(key);
    }

    private static bool ShouldEmitEntityDeath(string key)
    {
        var now = DateTimeOffset.UtcNow;
        lock (Sync)
        {
            foreach (var staleKey in RecentEntityDeaths.Where(entry => now - entry.Value > TimeSpan.FromSeconds(5)).Select(entry => entry.Key).ToArray())
            {
                RecentEntityDeaths.Remove(staleKey);
            }

            if (RecentEntityDeaths.ContainsKey(key))
            {
                return false;
            }

            RecentEntityDeaths[key] = now;
            return true;
        }
    }

    private static HitData? GetLastHit(Character character) =>
        LastHitField?.GetValue(character) as HitData;

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

[HarmonyPatch(typeof(Character), "OnDeath")]
internal static class TakaroCharacterOnDeathPatch
{
    private static void Postfix(Character __instance) =>
        ValheimChatEventBridge.EmitEntityKilled(__instance);
}
#else
namespace Takaro.Valheim.Plugin;
#endif
