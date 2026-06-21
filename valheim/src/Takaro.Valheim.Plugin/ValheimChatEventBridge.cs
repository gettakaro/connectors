using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using HarmonyLib;
using System.Text.Json;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

internal static class ValheimChatEventBridge
{
    private static readonly int ChatMessageHash = "ChatMessage".GetStableHashCode();
    private static readonly int SayHash = "Say".GetStableHashCode();
    private static readonly object Sync = new();
    private static readonly Dictionary<string, DateTimeOffset> RecentEvents = new(StringComparer.Ordinal);
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
        log($"Takaro Valheim chat hash diagnostics: ChatMessage={ChatMessageHash}, Say={SayHash}.");
    }

    public static void Shutdown()
    {
        runner = null;
        registeredRpc = null;
        lock (Sync)
        {
            RecentEvents.Clear();
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
        log("Takaro Valheim server-side chat and entity bridge active.");
    }

    public static void Emit(long senderId, int chatType, UserInfo userInfo, string text)
    {
        if (string.IsNullOrWhiteSpace(text) || IsDuplicate(senderId, chatType, userInfo, text))
        {
            return;
        }

        var activeRunner = runner;
        if (activeRunner is null)
        {
            return;
        }

        var player = PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
            FirstNonEmpty(userInfo.Name, userInfo.GetDisplayName(), senderId.ToString()),
            FirstNonEmpty(userInfo.UserId.ToString(), senderId.ToString()),
            null,
            null,
            null));
        var evt = EventFactory.ChatMessage(player, ChannelName(chatType), DateTimeOffset.UtcNow, text);
        log($"Takaro Valheim chat event captured: player={player.Name}, channel={ChannelName(chatType)}, msgLength={text.Length}.");

        _ = SendGameEventAsync(
            activeRunner,
            "chat-message",
            evt,
            "Takaro Valheim chat event sent to Takaro.",
            "Takaro Valheim chat event send failed");
    }

    public static void EmitLog(string level, string message)
    {
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
            data.m_parameters.SetPos(0);

            if (data.m_methodHash == ChatMessageHash)
            {
                log("Takaro Valheim observed routed ChatMessage packet.");
                _ = data.m_parameters.ReadVector3();
                var chatType = data.m_parameters.ReadInt();
                var userInfo = new UserInfo();
                userInfo.Deserialize(ref data.m_parameters);
                var text = data.m_parameters.ReadString();
                Emit(data.m_senderPeerID, chatType, userInfo, text);
                return;
            }

            if (data.m_methodHash == SayHash)
            {
                log("Takaro Valheim observed routed Say packet.");
                var chatType = data.m_parameters.ReadInt();
                var userInfo = new UserInfo();
                userInfo.Deserialize(ref data.m_parameters);
                var text = data.m_parameters.ReadString();
                Emit(data.m_senderPeerID, chatType, userInfo, text);
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
            log($"Takaro Valheim could not inspect routed chat packet: {ex.Message}");
        }
        finally
        {
            package.SetPos(originalPosition);
        }
    }

    internal static void EmitEntityKilled(Character character)
    {
        if (character is Player || character.GetComponent<Player>() != null || !ShouldEmitEntityDeath(character))
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
            var hit = GetLastHit(character);
            var killer = TryMapCharacterToTakaroPlayer(hit?.GetAttacker(), out var player)
                ? player
                : null;
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

    private static bool IsDuplicate(long senderId, int chatType, UserInfo userInfo, string text)
    {
        var now = DateTimeOffset.UtcNow;
        var key = $"{senderId}|{chatType}|{userInfo.UserId}|{userInfo.Name}|{text}";

        lock (Sync)
        {
            foreach (var staleKey in RecentEvents.Where(entry => now - entry.Value > TimeSpan.FromSeconds(2)).Select(entry => entry.Key).ToArray())
            {
                RecentEvents.Remove(staleKey);
            }

            if (RecentEvents.ContainsKey(key))
            {
                return true;
            }

            RecentEvents[key] = now;
            return false;
        }
    }

    private static string ChannelName(int chatType) =>
        (Talker.Type)chatType switch
        {
            Talker.Type.Whisper => "team",
            _ => "global"
        };

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

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

[HarmonyPatch(typeof(Chat), "RPC_ChatMessage")]
internal static class TakaroChatRpcChatMessagePatch
{
    private static bool Prefix(long sender, int type, UserInfo userInfo, string text)
    {
        ValheimChatEventBridge.Emit(sender, type, userInfo, text);
        return true;
    }
}

[HarmonyPatch(typeof(Talker), "RPC_Say")]
internal static class TakaroTalkerRpcSayPatch
{
    private static bool Prefix(long sender, int ctype, UserInfo user, string text)
    {
        ValheimChatEventBridge.Emit(sender, ctype, user, text);
        return true;
    }
}

[HarmonyPatch(typeof(ZRoutedRpc), "RPC_RoutedRPC")]
internal static class TakaroRoutedRpcPatch
{
    private static void Prefix(ZPackage pkg) =>
        ValheimChatEventBridge.ObserveRoutedRpc(pkg);
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
