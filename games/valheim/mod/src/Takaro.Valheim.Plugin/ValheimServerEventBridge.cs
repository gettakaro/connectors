using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using HarmonyLib;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Turns what the dedicated server itself sees into Takaro events, with no client mod:
/// chat lines and deaths from routed RPCs bound to the peer they arrived on, and creature
/// kills from the creature's network object when its owner destroys it. Hook bodies only read
/// the packet and hand the event to the WebSocket runner, which sends it off the game thread.
/// </summary>
internal static class ValheimServerEventBridge
{
    private static readonly int ChatMessageHash = "ChatMessage".GetStableHashCode();
    private static readonly int SayHash = "Say".GetStableHashCode();
    private static readonly int OnDeathHash = "OnDeath".GetStableHashCode();
    private static readonly int RegisterKillHash = "RPC_RegisterKill".GetStableHashCode();
    private static readonly TimeSpan CompanionVerdictWait = TimeSpan.FromSeconds(3);
    private static readonly List<PendingKill> PendingKills = new();
    private static readonly ServerChatRelayPolicy ChatPolicy = new();
    private static readonly OnceWithinWindow Deaths = new(TimeSpan.FromSeconds(5));
    private static TakaroWebSocketRunner? runner;
    private static ValheimPlayerResolver? resolver;
    private static Action<string> log = _ => { };
    private static int rejectedLogsRemaining = 50;

    [ThreadStatic]
    private static ZNetPeer? arrivalPeer;

    public static void Initialize(TakaroWebSocketRunner? activeRunner, ValheimPlayerResolver playerResolver, Action<string>? logger)
    {
        runner = activeRunner;
        resolver = playerResolver;
        log = logger ?? (_ => { });
        log("Takaro Valheim server-side events active: chat-message and player-death from peer-bound routed RPCs, entity-killed from the game's own kill reports.");
    }

    public static void Shutdown()
    {
        runner = null;
        resolver = null;
    }

    public static void EmitLog(string level, string message)
    {
        if (!ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.Log, ValheimEventObservationSource.Connector))
        {
            return;
        }

        Send(ValheimEventType.Log, EventFactory.Log(level, message, DateTimeOffset.UtcNow), successLog: null);
    }

    internal static void OnRoutedRpcArrived(ZRpc rpc, ZPackage package)
    {
        arrivalPeer = FindPeer(rpc);
        var start = package.GetPos();
        try
        {
            var data = new ZRoutedRpc.RoutedRPCData();
            data.Deserialize(package);
            if (data.m_methodHash != SayHash && data.m_methodHash != ChatMessageHash && data.m_methodHash != OnDeathHash && data.m_methodHash != RegisterKillHash)
            {
                return;
            }

            var peer = arrivalPeer;
            var origin = new RoutedPacketOrigin(
                peer?.m_uid ?? 0,
                data.m_senderPeerID,
                peer is null || peer.m_characterID.IsNone() ? null : peer.m_characterID.ToString(),
                data.m_targetZDO.IsNone() ? null : data.m_targetZDO.ToString());

            if (data.m_methodHash == RegisterKillHash)
            {
                ObserveKillReport(peer, origin, data);
            }
            else if (data.m_methodHash == OnDeathHash)
            {
                ObserveDeath(peer, origin, data);
            }
            else
            {
                ObserveChat(peer, origin, data);
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not inspect a routed packet: {ex.Message}");
        }
        finally
        {
            package.SetPos(start);
        }
    }

    internal static void OnRoutedRpcDone() => arrivalPeer = null;

    private static void ObserveChat(ZNetPeer? peer, RoutedPacketOrigin origin, ZRoutedRpc.RoutedRPCData data)
    {
        var parameters = data.m_parameters;
        parameters.SetPos(0);
        var characterScoped = data.m_methodHash == SayHash;
        if (!characterScoped)
        {
            parameters.ReadVector3();
        }

        var talkerType = parameters.ReadInt();
        parameters.ReadString();
        parameters.ReadString();
        var text = parameters.ReadString();

        var decision = ChatPolicy.Evaluate(origin, characterScoped, talkerType, text, DateTimeOffset.UtcNow);
        if (!decision.Emit)
        {
            if (decision.Reason.StartsWith("unbound", StringComparison.Ordinal))
            {
                LogRejected($"Takaro Valheim rejected a chat packet ({decision.Reason}): arrivalPeer={origin.ArrivalPeerUid}, claimedSender={origin.ClaimedSenderPeerId}, targetZdo={origin.TargetZdo ?? "none"}.");
            }

            return;
        }

        if (!ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.ChatMessage, ValheimEventObservationSource.PeerBoundRoutedRpc)
            || peer is null
            || resolver is null
            || !resolver.TryResolvePeerPlayer(peer, out var player)
            || player is null)
        {
            return;
        }

        Send(
            ValheimEventType.ChatMessage,
            EventFactory.ChatMessage(player, "global", DateTimeOffset.UtcNow, decision.Text),
            $"Takaro Valheim chat-message queued (server-bound) for {player.Name} ({player.GameId}).");
    }

    // The game that simulated a creature reports its death to every player marked as having
    // hit it (Character.OnDeath -> Game.RegisterKill). Reports for other players are routed
    // through the server to them; the report for that game's own player reaches the server
    // through the kill witness entry in its player list (TakaroChatParticipant.WitnessFor).
    private static void ObserveKillReport(ZNetPeer? peer, RoutedPacketOrigin origin, ZRoutedRpc.RoutedRPCData data)
    {
        if (peer is null || RoutedPacketBindingPolicy.Evaluate(origin, requireOwnCharacter: false) != RoutedBindingResult.Bound)
        {
            LogRejected($"Takaro Valheim rejected a kill report (unbound): arrivalPeer={origin.ArrivalPeerUid}, claimedSender={origin.ClaimedSenderPeerId}.");
            return;
        }

        ZNetPeer? killer;
        string path;
        if (TakaroChatParticipant.IsKillWitnessTarget(data.m_targetPeerID))
        {
            killer = peer;
            path = OwnKill;
        }
        else
        {
            killer = ZNet.instance?.GetPeer(data.m_targetPeerID);
            path = "relayed to the killer";
        }

        data.m_parameters.SetPos(0);
        var enemyToken = data.m_parameters.ReadString();
        data.m_parameters.ReadInt();
        var modifier = data.m_parameters.ReadInt();
        var attackers = data.m_parameters.ReadInt();
        if (killer is null
            || resolver is null
            || !resolver.TryResolvePeerPlayer(killer, out var player)
            || player is null)
        {
            return;
        }

        var entity = ValheimLocalizer.Localize(enemyToken, ValheimDisplayName.FromToken(enemyToken.TrimStart('$'), enemyToken));
        var weapon = KillWeaponPolicy.Describe(
            modifier,
            EquippedItemName(killer, ZDOVars.s_rightItem),
            EquippedItemName(killer, ZDOVars.s_leftItem));
        var kill = new PendingKill(killer.m_uid, player, enemyToken, entity, weapon, path, attackers, DateTimeOffset.UtcNow);

        // Valheim credits every player who hit the creature, even when something else dealt
        // the last blow, and the last hit never leaves the simulating game. A player who runs
        // the optional companion reports it; for that player's own kills the event waits up to
        // 3 s for the report. Without a companion the game's own credit is used as is.
        if (path == OwnKill && CompanionKillVerdicts.HasSession(killer.m_uid))
        {
            PendingKills.Add(kill);
            return;
        }

        Emit(kill, "server: game kill credit");
    }

    private const string OwnKill = "own kill";

    private sealed class PendingKill
    {
        public PendingKill(long killerUid, TakaroPlayer player, string enemyToken, string entity, string weapon, string path, int attackers, DateTimeOffset at)
        {
            KillerUid = killerUid;
            Player = player;
            EnemyToken = enemyToken;
            Entity = entity;
            Weapon = weapon;
            Path = path;
            Attackers = attackers;
            At = at;
        }

        public long KillerUid { get; }
        public TakaroPlayer Player { get; }
        public string EnemyToken { get; }
        public string Entity { get; }
        public string Weapon { get; }
        public string Path { get; }
        public int Attackers { get; }
        public DateTimeOffset At { get; }
    }

    /// <summary>Called from the plugin's Update; only works while a kill waits for a companion report.</summary>
    internal static void Update()
    {
        if (PendingKills.Count == 0)
        {
            return;
        }

        var now = DateTimeOffset.UtcNow;
        for (var i = PendingKills.Count - 1; i >= 0; i--)
        {
            var kill = PendingKills[i];
            if (CompanionKillVerdicts.TryTake(kill.KillerUid, kill.EnemyToken, kill.At, CompanionVerdictWait, out var lastHitByPlayer, out var lastHitKind))
            {
                PendingKills.RemoveAt(i);
                if (lastHitByPlayer)
                {
                    Emit(kill, "server: game kill credit, last hit confirmed by companion");
                }
                else
                {
                    log($"Takaro Valheim did not credit {kill.Player.Name} with {kill.Entity}: the companion reports the last hit came from {lastHitKind} (client-reported).");
                }
            }
            else if (now - kill.At >= CompanionVerdictWait)
            {
                PendingKills.RemoveAt(i);
                Emit(kill, "server: game kill credit, no companion report");
            }
        }
    }

    private static void Emit(PendingKill kill, string source)
    {
        if (!ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.EntityKilled, ValheimEventObservationSource.GameKillReport))
        {
            return;
        }

        Send(
            ValheimEventType.EntityKilled,
            EventFactory.EntityKilled(kill.Player, kill.Entity, kill.At, kill.Weapon),
            $"Takaro Valheim entity-killed queued ({source}; {kill.Path}, {kill.Attackers} attacker(s)) for {kill.Player.Name}: {kill.Entity} with {kill.Weapon}.");
    }

    private static void ObserveDeath(ZNetPeer? peer, RoutedPacketOrigin origin, ZRoutedRpc.RoutedRPCData data)
    {
        var binding = RoutedPacketBindingPolicy.Evaluate(origin, requireOwnCharacter: true);
        if (binding != RoutedBindingResult.Bound)
        {
            LogRejected($"Takaro Valheim rejected an OnDeath packet ({binding}): arrivalPeer={origin.ArrivalPeerUid}, claimedSender={origin.ClaimedSenderPeerId}, targetZdo={origin.TargetZdo ?? "none"}.");
            return;
        }

        if (!Deaths.TryAccept($"{origin.ArrivalPeerUid}|{origin.TargetZdo}", DateTimeOffset.UtcNow)
            || !ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.PlayerDeath, ValheimEventObservationSource.PeerBoundRoutedRpc)
            || peer is null
            || resolver is null
            || !resolver.TryResolvePeerPlayer(peer, out var player)
            || player is null)
        {
            return;
        }

        var zdo = ZDOMan.instance?.GetZDO(data.m_targetZDO);
        var where = zdo?.GetPosition() ?? peer.m_refPos;
        Send(
            ValheimEventType.PlayerDeath,
            EventFactory.PlayerDeath(player, DateTimeOffset.UtcNow, new TakaroPosition(where.x, where.y, where.z, "valheim"), attacker: null, weapon: null),
            $"Takaro Valheim player-death queued (server-bound) for {player.Name} ({player.GameId}).");
    }

    private static string? EquippedItemName(ZNetPeer peer, int slotHash)
    {
        var characterZdo = ZDOMan.instance.GetZDO(peer.m_characterID);
        var itemHash = characterZdo?.GetInt(slotHash) ?? 0;
        if (itemHash == 0 && characterZdo is not null)
        {
            itemHash = slotHash == ZDOVars.s_rightItem
                ? characterZdo.GetInt(ZDOVars.s_rightBackItem)
                : slotHash == ZDOVars.s_leftItem ? characterZdo.GetInt(ZDOVars.s_leftBackItem) : 0;
        }

        if (itemHash == 0 || ObjectDB.instance is null)
        {
            return null;
        }

        var item = ObjectDB.instance.GetItemPrefab(itemHash);
        if (item is null || !item.TryGetComponent<ItemDrop>(out var drop))
        {
            return null;
        }

        var shared = drop.m_itemData?.m_shared;
        if (shared is null || shared.m_itemType is ItemDrop.ItemData.ItemType.Shield or ItemDrop.ItemData.ItemType.Torch)
        {
            return null;
        }

        return ValheimLocalizer.Localize(shared.m_name, item.name);
    }

    private static ZNetPeer? FindPeer(ZRpc rpc)
    {
        foreach (var peer in ZNet.instance?.GetPeers() ?? new List<ZNetPeer>())
        {
            if (ReferenceEquals(peer.m_rpc, rpc))
            {
                return peer;
            }
        }

        return null;
    }

    private static void LogRejected(string message)
    {
        if (rejectedLogsRemaining > 0)
        {
            rejectedLogsRemaining--;
            log(message);
        }
    }

    private static void Send(string eventType, object evt, string? successLog)
    {
        var activeRunner = runner;
        if (activeRunner is null)
        {
            return;
        }

        _ = SendAsync(activeRunner, eventType, evt, successLog);
    }

    private static async Task SendAsync(TakaroWebSocketRunner activeRunner, string eventType, object evt, string? successLog)
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
            log($"Takaro Valheim {eventType} event send failed: {ex.Message}");
        }
    }
}

[HarmonyPatch(typeof(ZRoutedRpc), "RPC_RoutedRPC")]
internal static class TakaroRoutedRpcArrivalPatch
{
    private static void Prefix(ZRpc rpc, ZPackage pkg)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimServerEventBridge.OnRoutedRpcArrived(rpc, pkg);
        }
    }

    private static void Finalizer() => ValheimServerEventBridge.OnRoutedRpcDone();
}

// The dedicated server has no player of its own; a kill report addressed to it (the kill
// witness) must not touch the server's placeholder profile.
[HarmonyPatch(typeof(Game), "RPC_RegisterKill")]
internal static class TakaroServerKillReportPatch
{
    private static bool Prefix() => !(ZNet.instance is not null && ZNet.instance.IsDedicated());
}

#else
namespace Takaro.Valheim.Plugin;
#endif
