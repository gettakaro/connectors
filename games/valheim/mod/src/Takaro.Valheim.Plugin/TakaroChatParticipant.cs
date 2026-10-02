#if TAKARO_VALHEIM_PLUGIN
using HarmonyLib;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// A vanilla Valheim client sends every chat line once per entry of the player list the
/// server gives it, and only shows chat from senders that are on that list. The dedicated
/// server is not on it, so it neither hears a player who is alone nor can it speak. This adds
/// one "Takaro" entry, owned by the server, to the player list sent to clients: their chat
/// lines then also travel to the server, and server messages render as normal chat.
/// The entry is only ever written into the outgoing packet; the server's own player list,
/// player history and Takaro player list never contain it.
/// </summary>
internal static class TakaroChatParticipant
{
    public const string PlatformUserId = "Takaro_server";
    private const uint CharacterObjectId = 0x7A4B0001;
    private static readonly Vector3 AboveHead = new(0f, 2.2f, 0f);
    private static string name = "Takaro";
    private static bool formatVerified;
    private static bool formatMismatchLogged;
    private static Action<string> log = _ => { };

    public static bool Active { get; private set; }

    public static string Name => name;

    public static void Initialize(string displayName, Action<string> logger)
    {
        name = string.IsNullOrWhiteSpace(displayName) ? "Takaro" : displayName.Trim();
        currentListedName = name;
        log = logger;
    }

    public static long ServerUid => ZDOMan.GetSessionID();

    public static bool IsParticipantTarget(long targetPeerId) =>
        Active && targetPeerId != 0 && targetPeerId == ServerUid;

    /// <summary>Changes the shown sender name and re-sends the player list when it differs.</summary>
    public static void EnsureName(string displayName)
    {
        var wanted = string.IsNullOrWhiteSpace(displayName) ? name : displayName.Trim();
        if (wanted == currentListedName)
        {
            return;
        }

        currentListedName = wanted;
        var znet = ZNet.instance;
        if (znet is not null)
        {
            AccessTools.Method(typeof(ZNet), "SendPlayerList")?.Invoke(znet, Array.Empty<object>());
        }
    }

    private static string currentListedName = "Takaro";

    /// <summary>
    /// Sends one chat line from the participant to one peer. Valheim also shows every chat line
    /// as a short-lived floating text at the speaker's position; with no speaker in the world,
    /// it is placed just above the recipient instead of being pinned to a screen edge.
    /// </summary>
    public static void Send(ZNetPeer peer, string text)
    {
        var info = new UserInfo
        {
            Name = currentListedName,
            UserId = new Splatform.PlatformUserID(PlatformUserId)
        };
        ZRoutedRpc.instance.InvokeRoutedRPC(peer.m_uid, "ChatMessage", peer.m_refPos + AboveHead, (int)Talker.Type.Normal, info, text);
    }

    /// <summary>
    /// Replaces vanilla SendPlayerList on the dedicated server: every ready peer gets the
    /// vanilla list plus the "Takaro" chat entry plus a kill witness for that peer (see
    /// <see cref="WitnessFor"/>). Returns false, so vanilla runs instead, if the vanilla
    /// format cannot be reproduced byte for byte.
    /// </summary>
    internal static bool SendPerPeer(ZNet znet)
    {
        try
        {
            AccessTools.Method(typeof(ZNet), "UpdatePlayerList")?.Invoke(znet, Array.Empty<object>());
            var peers = znet.GetPeers();
            if (peers.Count == 0)
            {
                return true;
            }

            var players = znet.GetPlayerList();
            if (!formatVerified)
            {
                var vanilla = AccessTools.Method(typeof(ZNet), "WritePlayerInfo")?.Invoke(znet, new object[] { players }) as ZPackage;
                if (vanilla is null || !Write(players, false, null).GetArray().SequenceEqual(vanilla.GetArray()))
                {
                    if (!formatMismatchLogged)
                    {
                        formatMismatchLogged = true;
                        log("Takaro Valheim chat participant disabled: this Valheim build writes its player list in an unknown format, so player chat, server messages and kill reports are unavailable.");
                    }

                    Active = false;
                    return false;
                }

                formatVerified = true;
                log($"Takaro Valheim chat participant '{currentListedName}' active (server-side chat relay and kill witness).");
            }

            Active = true;
            for (var index = 0; index < peers.Count; index++)
            {
                var peer = peers[index];
                if (peer.IsReady())
                {
                    peer.m_rpc.Invoke("PlayerList", Write(players, true, WitnessFor(peer, players)));
                }
            }

            return true;
        }
        catch (Exception ex)
        {
            Active = false;
            log($"Takaro Valheim chat participant skipped: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// When a creature dies, the game that simulated it sends a kill report to every player
    /// whose name is marked on the creature as having hit it (Character.OnDeath ->
    /// Game.RegisterKill). The report for that game's own player stays local, so the server
    /// never saw solo kills. The witness is a second entry for the receiving player, with the
    /// same name and platform id but a character id owned by the server: the game then also
    /// addresses its own player's kill report to the server. Same platform id keeps the
    /// in-game player list from showing a second row.
    /// </summary>
    internal static ZNet.PlayerInfo? WitnessFor(ZNetPeer peer, List<ZNet.PlayerInfo> players)
    {
        foreach (var player in players)
        {
            if (!peer.m_characterID.IsNone() && player.m_characterID == peer.m_characterID)
            {
                var witness = player;
                witness.m_characterID = new ZDOID(ServerUid, KillWitnessBase + (uint)(peer.m_uid & 0xFFFF));
                witness.m_publicPosition = false;
                return witness;
            }
        }

        return null;
    }

    public const uint KillWitnessBase = 0x7A4C0000;

    public static bool IsKillWitnessTarget(long targetPeerId) => Active && targetPeerId != 0 && targetPeerId == ServerUid;

    private static ZPackage Write(List<ZNet.PlayerInfo> players, bool participants, ZNet.PlayerInfo? witness)
    {
        var extra = participants ? (witness is null ? 1 : 2) : 0;
        var package = new ZPackage();
        package.Write(players.Count + extra);
        foreach (var player in players)
        {
            WriteEntry(package, player);
        }

        if (participants)
        {
            WriteEntry(
                package,
                currentListedName,
                new ZDOID(ServerUid, CharacterObjectId),
                PlatformUserId,
                currentListedName,
                currentListedName,
                string.Empty,
                false,
                Vector3.zero);
            if (witness is { } self)
            {
                WriteEntry(package, self);
            }
        }

        return package;
    }

    private static void WriteEntry(ZPackage package, ZNet.PlayerInfo player) =>
        WriteEntry(
            package,
            player.m_name,
            player.m_characterID,
            player.m_userInfo.m_id.ToString(),
            player.m_userInfo.m_displayName,
            player.m_userInfo.m_serverAssignedDisplayName,
            player.m_userInfo.m_playfabId,
            player.m_publicPosition,
            player.m_position);

    private static void WriteEntry(
        ZPackage package,
        string name,
        ZDOID characterId,
        string platformUserId,
        string? displayName,
        string? serverAssignedDisplayName,
        string? playfabId,
        bool publicPosition,
        Vector3 position)
    {
        package.Write(name ?? string.Empty);
        package.Write(characterId);
        package.Write(platformUserId ?? string.Empty);
        package.Write(displayName ?? string.Empty);
        package.Write(serverAssignedDisplayName ?? string.Empty);
        package.Write(playfabId ?? string.Empty);
        package.Write(publicPosition);
        if (publicPosition)
        {
            package.Write(position);
        }
    }
}

[HarmonyPatch(typeof(ZNet), "SendPlayerList")]
internal static class TakaroPlayerListPatch
{
    private static bool Prefix(ZNet __instance) =>
        !(__instance.IsDedicated() && TakaroChatParticipant.SendPerPeer(__instance));
}
#endif
