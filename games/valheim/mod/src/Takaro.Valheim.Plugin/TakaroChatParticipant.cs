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

    internal static ZPackage AppendTo(List<ZNet.PlayerInfo> players, ZPackage vanilla)
    {
        try
        {
            var rebuilt = Write(players, includeParticipant: false);
            if (!formatVerified)
            {
                if (!rebuilt.GetArray().SequenceEqual(vanilla.GetArray()))
                {
                    if (!formatMismatchLogged)
                    {
                        formatMismatchLogged = true;
                        log("Takaro Valheim chat participant disabled: this Valheim build writes its player list in an unknown format, so player chat and server messages are unavailable.");
                    }

                    Active = false;
                    return vanilla;
                }

                formatVerified = true;
                log($"Takaro Valheim chat participant '{currentListedName}' active (server-side chat relay).");
            }

            Active = true;
            return Write(players, includeParticipant: true);
        }
        catch (Exception ex)
        {
            Active = false;
            log($"Takaro Valheim chat participant skipped: {ex.Message}");
            return vanilla;
        }
    }

    private static ZPackage Write(List<ZNet.PlayerInfo> players, bool includeParticipant)
    {
        var package = new ZPackage();
        package.Write(players.Count + (includeParticipant ? 1 : 0));
        foreach (var player in players)
        {
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
        }

        if (includeParticipant)
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
        }

        return package;
    }

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

[HarmonyPatch(typeof(ZNet), "WritePlayerInfo")]
internal static class TakaroPlayerListPatch
{
    private static void Postfix(List<ZNet.PlayerInfo> playerInfoList, ref ZPackage __result)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            __result = TakaroChatParticipant.AppendTo(playerInfoList, __result);
        }
    }
}
#endif
