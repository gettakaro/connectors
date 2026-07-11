using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx.Logging;

namespace Takaro.Valheim.Plugin;

public sealed class ValheimPlayerResolver
{
    private readonly ManualLogSource logger;

    public ValheimPlayerResolver(ManualLogSource logger)
    {
        this.logger = logger ?? throw new ArgumentNullException(nameof(logger));
    }

    public TakaroPlayer ToTakaroPlayer(ZNet.PlayerInfo player)
    {
        var playerId = FirstNonEmpty(player.m_userInfo.m_id.ToString(), player.m_characterID.ToString());
        var takaroPlayer = PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
            FirstNonEmpty(player.m_name, player.m_serverAssignedDisplayName, player.m_userInfo.m_displayName, playerId),
            playerId,
            null,
            null,
            null));

        logger.LogInfo($"Takaro Valheim player mapped: name={takaroPlayer.Name}, gameId={takaroPlayer.GameId}, platformId={takaroPlayer.PlatformId ?? "<none>"}.");
        return takaroPlayer;
    }

    public TakaroPlayer ToTakaroPlayer(ZNetPeer peer)
    {
        var hostName = peer.m_socket.GetHostName();
        var platformId = hostName.Contains('_') ? hostName : $"Steam_{hostName}";
        return PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
            FirstNonEmpty(peer.m_playerName, platformId),
            platformId,
            null,
            null,
            null));
    }

    public bool TryResolvePlayer(
        string identifier,
        out ZNet.PlayerInfo playerInfo,
        out ZNetPeer? peer,
        out TakaroPlayer? player)
    {
        if (TryFindPlayerInfo(identifier, out playerInfo, out var playerInfoAmbiguous))
        {
            player = ToTakaroPlayer(playerInfo);
            peer = TryFindPeer(playerInfo, player, out var resolvedPeer) ? resolvedPeer : null;
            return true;
        }

        if (playerInfoAmbiguous)
        {
            playerInfo = default;
            peer = null;
            player = null;
            return false;
        }

        var peerCandidates = (ZNet.instance?.GetPeers() ?? [])
            .Select(candidate => new
            {
                Source = candidate,
                Player = ToTakaroPlayer(candidate)
            })
            .ToArray();
        var peerFound = PlayerMapper.TryFindUnique(
            peerCandidates.Select(candidate => candidate.Player),
            identifier,
            out player,
            out var peerAmbiguous);
        if (peerFound && !peerAmbiguous && player is not null)
        {
            playerInfo = default;
            var resolvedPeerPlayer = player;
            peer = peerCandidates
                .First(candidate => ReferenceEquals(candidate.Player, resolvedPeerPlayer))
                .Source;
            return true;
        }

        playerInfo = default;
        peer = null;
        player = null;
        return false;
    }

    public bool TryResolveConnectedPeer(
        long sender,
        out ZNetPeer? peer,
        out TakaroPlayer? player)
    {
        foreach (var candidate in ZNet.instance?.GetPeers() ?? [])
        {
            if (candidate.m_uid == sender)
            {
                peer = candidate;
                player = ToTakaroPlayer(candidate);
                return true;
            }
        }

        peer = null;
        player = null;
        return false;
    }

    private bool TryFindPlayerInfo(
        string identifier,
        out ZNet.PlayerInfo player,
        out bool ambiguous)
    {
        var candidates = (ZNet.instance?.GetPlayerList() ?? [])
            .Select(candidate => new
            {
                Source = candidate,
                Player = ToTakaroPlayer(candidate)
            })
            .ToArray();
        if (!PlayerMapper.TryFindUnique(
                candidates.Select(candidate => candidate.Player),
                identifier,
                out var resolvedPlayer,
                out ambiguous)
            || resolvedPlayer is null)
        {
            player = default;
            return false;
        }

        player = candidates
            .First(candidate => ReferenceEquals(candidate.Player, resolvedPlayer))
            .Source;
        return true;
    }

    private bool TryFindPeer(ZNet.PlayerInfo playerInfo, TakaroPlayer player, out ZNetPeer peer)
    {
        foreach (var candidate in ZNet.instance?.GetPeers() ?? [])
        {
            if ((candidate.m_characterID == playerInfo.m_characterID && !candidate.m_characterID.IsNone())
                || Matches(candidate.m_playerName, playerInfo.m_name)
                || Matches(candidate.m_playerName, player.Name)
                || Matches(candidate.m_socket.GetHostName(), playerInfo.m_userInfo.m_id.m_userID)
                || Matches(candidate.m_socket.GetHostName(), playerInfo.m_userInfo.m_id.ToString())
                || Matches(ToTakaroPlayer(candidate).GameId, player.GameId))
            {
                peer = candidate;
                return true;
            }
        }

        peer = null!;
        return false;
    }

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

    private static bool Matches(string? value, string? needle) =>
        !string.IsNullOrWhiteSpace(value)
        && !string.IsNullOrWhiteSpace(needle)
        && value!.Equals(needle, StringComparison.OrdinalIgnoreCase);
}
#else
namespace Takaro.Valheim.Plugin;
#endif
