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
    private static readonly int SetTriggerHash = "SetTrigger".GetStableHashCode();
    private static readonly Dictionary<long, DateTimeOffset> LastAttack = new();
    private static HashSet<string>? attackAnimations;
    private static readonly ServerChatRelayPolicy ChatPolicy = new();
    private static readonly OnceWithinWindow Deaths = new(TimeSpan.FromSeconds(5));
    private static readonly OnceWithinWindow Kills = new(TimeSpan.FromSeconds(30));
    private static TakaroWebSocketRunner? runner;
    private static ValheimPlayerResolver? resolver;
    private static Action<string> log = _ => { };
    private static int rejectedLogsRemaining = 50;
    private static readonly List<PendingDeath> PendingDeaths = new();
    private static readonly Dictionary<int, int[]> RagdollPrefabs = new();
    private static readonly OnceWithinWindow UsedRagdolls = new(TimeSpan.FromMinutes(2));
    private static float nextPendingCheck;

    private sealed class PendingDeath
    {
        public PendingDeath(ZDOID creature, long destroyerUid, Vector3 position, string prefabName, string entity, int[] ragdollHashes, DateTimeOffset destroyedAt)
        {
            Creature = creature;
            DestroyerUid = destroyerUid;
            Position = position;
            PrefabName = prefabName;
            Entity = entity;
            RagdollHashes = ragdollHashes;
            DestroyedAt = destroyedAt;
        }

        public ZDOID Creature { get; }
        public long DestroyerUid { get; }
        public Vector3 Position { get; }
        public string PrefabName { get; }
        public string Entity { get; }
        public int[] RagdollHashes { get; }
        public DateTimeOffset DestroyedAt { get; }
    }

    [ThreadStatic]
    private static ZNetPeer? arrivalPeer;

    public static void Initialize(TakaroWebSocketRunner? activeRunner, ValheimPlayerResolver playerResolver, Action<string>? logger)
    {
        runner = activeRunner;
        resolver = playerResolver;
        log = logger ?? (_ => { });
        log("Takaro Valheim server-side events active: chat-message and player-death from peer-bound routed RPCs, entity-killed from creature network objects.");
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
            if (data.m_methodHash != SayHash && data.m_methodHash != ChatMessageHash && data.m_methodHash != OnDeathHash && data.m_methodHash != SetTriggerHash)
            {
                return;
            }

            var peer = arrivalPeer;
            var origin = new RoutedPacketOrigin(
                peer?.m_uid ?? 0,
                data.m_senderPeerID,
                peer is null || peer.m_characterID.IsNone() ? null : peer.m_characterID.ToString(),
                data.m_targetZDO.IsNone() ? null : data.m_targetZDO.ToString());

            if (data.m_methodHash == SetTriggerHash)
            {
                ObserveAttack(peer, origin, data);
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

    internal static void OnDestroyZdo(long sender, ZPackage package)
    {
        var peer = arrivalPeer;
        if (peer is null || peer.m_uid != sender || ZDOMan.instance is null || ZNetScene.instance is null)
        {
            return;
        }

        var start = package.GetPos();
        try
        {
            var count = package.ReadInt();
            for (var i = 0; i < count && i < 4096; i++)
            {
                ObserveDestroyed(peer, package.ReadZDOID());
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not inspect destroyed objects: {ex.Message}");
        }
        finally
        {
            package.SetPos(start);
        }
    }

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
            $"Takaro Valheim chat-message forwarded (server-bound) for {player.Name} ({player.GameId}).");
    }

    // Animation triggers are broadcast for the player's own character; only weapon attack
    // animations (taken from the game's item definitions) count as an attack.
    private static void ObserveAttack(ZNetPeer? peer, RoutedPacketOrigin origin, ZRoutedRpc.RoutedRPCData data)
    {
        if (peer is null || RoutedPacketBindingPolicy.Evaluate(origin, requireOwnCharacter: true) != RoutedBindingResult.Bound)
        {
            return;
        }

        data.m_parameters.SetPos(0);
        var trigger = InferredKillPolicy.AttackAnimationBase(data.m_parameters.ReadString());
        if (trigger.Length > 0 && AttackAnimations().Contains(trigger))
        {
            LastAttack[peer.m_uid] = DateTimeOffset.UtcNow;
        }
    }

    private static HashSet<string> AttackAnimations()
    {
        if (attackAnimations is not null && attackAnimations.Count > 0)
        {
            return attackAnimations;
        }

        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in ObjectDB.instance?.m_items ?? new List<GameObject>())
        {
            var shared = item?.GetComponent<ItemDrop>()?.m_itemData?.m_shared;
            foreach (var attack in new[] { shared?.m_attack, shared?.m_secondaryAttack })
            {
                if (!string.IsNullOrWhiteSpace(attack?.m_attackAnimation))
                {
                    names.Add(attack!.m_attackAnimation);
                }
            }
        }

        attackAnimations = names;
        return names;
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
            $"Takaro Valheim player-death forwarded (server-bound) for {player.Name} ({player.GameId}).");
    }

    private static void ObserveDestroyed(ZNetPeer destroyer, ZDOID id)
    {
        var zdo = ZDOMan.instance.GetZDO(id);
        if (zdo is null)
        {
            return;
        }

        var prefab = ZNetScene.instance.GetPrefab(zdo.GetPrefab());
        if (prefab is null || !prefab.TryGetComponent<Character>(out var character) || prefab.GetComponent<Player>() is not null)
        {
            return;
        }

        var peers = (ZNet.instance?.GetPeers() ?? new List<ZNetPeer>())
            .Where(candidate => candidate.IsReady() && !candidate.m_characterID.IsNone() && !string.IsNullOrEmpty(candidate.m_playerName))
            .ToArray();
        var candidates = peers
            .Select(candidate => new KillCandidate(candidate.m_uid, zdo.GetBool(ZDOVars.s_attackers + candidate.m_playerName)))
            .ToArray();
        var attribution = KillAttributionPolicy.Choose(candidates, destroyer.m_uid);
        if (attribution.PeerUid is null)
        {
            if (attribution.Reason == "no-player-hit")
            {
                QueueForRagdollEvidence(destroyer, id, zdo, prefab, character);
            }
            else
            {
                log($"Takaro Valheim did not attribute the death of {prefab.name} ({attribution.Reason}).");
            }

            return;
        }

        var killer = peers.First(candidate => candidate.m_uid == attribution.PeerUid);
        if (!Kills.TryAccept(id.ToString(), DateTimeOffset.UtcNow)
            || !ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.EntityKilled, ValheimEventObservationSource.ServerZdoState)
            || resolver is null
            || !resolver.TryResolvePeerPlayer(killer, out var player)
            || player is null)
        {
            return;
        }

        var entity = ValheimLocalizer.Localize(character.m_name, prefab.name);
        var weapon = KillWeaponPolicy.Describe(
            zdo.GetInt(ZDOVars.s_modifiers, (int)ValheimKillModifier.CountNone),
            EquippedItemName(killer, ZDOVars.s_rightItem),
            EquippedItemName(killer, ZDOVars.s_leftItem));
        Send(
            ValheimEventType.EntityKilled,
            EventFactory.EntityKilled(player, entity, DateTimeOffset.UtcNow, weapon),
            $"Takaro Valheim entity-killed forwarded (server-observed, {attribution.Reason}) for {player.Name}: {entity} with {weapon}.");
    }

    /// <summary>
    /// Called from the plugin's Update. Does work only while a creature death waits for its
    /// ragdoll, at most twice a second, scanning the creature's own zone neighbourhood.
    /// </summary>
    internal static void Update()
    {
        if (PendingDeaths.Count == 0 || Time.realtimeSinceStartup < nextPendingCheck || ZDOMan.instance is null)
        {
            return;
        }

        nextPendingCheck = Time.realtimeSinceStartup + 0.5f;
        var now = DateTimeOffset.UtcNow;
        var nearby = new List<ZDO>();
        for (var i = PendingDeaths.Count - 1; i >= 0; i--)
        {
            var pending = PendingDeaths[i];
            nearby.Clear();
            ZDOMan.instance.FindSectorObjects(ZoneSystem.GetZone(pending.Position), new SimulationDistance(1, 0, classic: true), nearby);
            var ragdoll = nearby.FirstOrDefault(candidate =>
                candidate.m_uid.UserID == pending.DestroyerUid
                && pending.RagdollHashes.Contains(candidate.GetPrefab())
                && Vector3.Distance(candidate.GetPosition(), pending.Position) <= 6f
                && UsedRagdolls.TryAccept(candidate.m_uid.ToString(), now));
            if (ragdoll is not null)
            {
                PendingDeaths.RemoveAt(i);
                EmitInferredKill(pending);
            }
            else if (!InferredKillPolicy.StillWaiting(pending.DestroyedAt, now))
            {
                PendingDeaths.RemoveAt(i);
            }
        }
    }

    private static void QueueForRagdollEvidence(ZNetPeer destroyer, ZDOID id, ZDO zdo, GameObject prefab, Character character)
    {
        var hashes = RagdollHashesFor(prefab, character);
        if (hashes.Length == 0 || PendingDeaths.Count >= 64)
        {
            return;
        }

        PendingDeaths.Add(new PendingDeath(
            id,
            destroyer.m_uid,
            zdo.GetPosition(),
            prefab.name,
            ValheimLocalizer.Localize(character.m_name, prefab.name),
            hashes,
            DateTimeOffset.UtcNow));
    }

    private static int[] RagdollHashesFor(GameObject prefab, Character character)
    {
        var key = prefab.name.GetStableHashCode();
        if (RagdollPrefabs.TryGetValue(key, out var cached))
        {
            return cached;
        }

        var hashes = (character.m_deathEffects?.m_effectPrefabs ?? Array.Empty<EffectList.EffectData>())
            .Where(effect => effect?.m_prefab is not null && effect.m_prefab.GetComponent<Ragdoll>() is not null)
            .Select(effect => effect.m_prefab.name.GetStableHashCode())
            .Distinct()
            .ToArray();
        RagdollPrefabs[key] = hashes;
        return hashes;
    }

    private static void EmitInferredKill(PendingDeath pending)
    {
        var killer = (ZNet.instance?.GetPeers() ?? new List<ZNetPeer>())
            .FirstOrDefault(candidate => candidate.m_uid == pending.DestroyerUid && candidate.IsReady() && !candidate.m_characterID.IsNone());
        var distance = killer is null ? float.NaN : DistanceToPlayer(killer, pending.Position);
        var attacked = killer is not null && LastAttack.TryGetValue(killer.m_uid, out var attackAt)
            && InferredKillPolicy.AttackedJustBefore(attackAt, pending.DestroyedAt);
        if (killer is null
            || !attacked
            || !InferredKillPolicy.KillerCloseEnough(distance)
            || !Kills.TryAccept(pending.Creature.ToString(), DateTimeOffset.UtcNow)
            || resolver is null
            || !resolver.TryResolvePeerPlayer(killer, out var player)
            || player is null)
        {
            log($"Takaro Valheim saw {pending.PrefabName} die but did not credit a kill (attack seen: {attacked}, distance: {distance:0.0} m).");
            return;
        }

        var weapon = KillWeaponPolicy.Describe(
            (int)ValheimKillModifier.CountNone,
            EquippedItemName(killer, ZDOVars.s_rightItem),
            EquippedItemName(killer, ZDOVars.s_leftItem));
        Send(
            ValheimEventType.EntityKilled,
            EventFactory.EntityKilled(player, pending.Entity, DateTimeOffset.UtcNow, weapon),
            $"Takaro Valheim entity-killed forwarded (server-observed, single-blow: attack + ragdoll {distance:0.0} m from {player.Name}): {pending.Entity} with {weapon}.");
    }

    // The player's own reference position is refreshed by the client every frame it moves; the
    // character object's position on the server can lag behind it.
    private static float DistanceToPlayer(ZNetPeer peer, Vector3 position)
    {
        var best = peer.m_refPos != Vector3.zero ? Vector3.Distance(peer.m_refPos, position) : float.NaN;
        var characterZdo = ZDOMan.instance.GetZDO(peer.m_characterID);
        if (characterZdo is not null)
        {
            var fromCharacter = Vector3.Distance(characterZdo.GetPosition(), position);
            best = float.IsNaN(best) ? fromCharacter : Math.Min(best, fromCharacter);
        }

        return best;
    }

    // A drawn weapon is in the hand slot; a sheathed one moves to the matching back slot.
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

[HarmonyPatch(typeof(ZDOMan), "RPC_DestroyZDO")]
internal static class TakaroDestroyZdoPatch
{
    private static void Prefix(long sender, ZPackage pkg)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimServerEventBridge.OnDestroyZdo(sender, pkg);
        }
    }
}
#else
namespace Takaro.Valheim.Plugin;
#endif
