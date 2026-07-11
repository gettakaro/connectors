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

        logger.LogDebug($"Takaro Valheim player mapped: name={takaroPlayer.Name}, gameId={takaroPlayer.GameId}, platformId={takaroPlayer.PlatformId ?? "<none>"}.");
        return takaroPlayer;
    }

    public TakaroPlayer ToTakaroPlayer(ZNetPeer peer)
    {
        var hostName = peer.m_socket?.GetHostName();
        hostName = FirstNonEmpty(hostName, peer.m_uid.ToString());
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

        var peerCandidates = GetPeerCandidates()
            .Where(candidate => candidate.IsReady)
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
        if (PeerResolutionPolicy.TryResolveReadySender(
                GetPeerCandidates(),
                sender,
                out var resolved)
            && resolved is not null)
        {
            peer = resolved.Source;
            player = resolved.Player;
            return true;
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
        var characterId = playerInfo.m_characterID.IsNone()
            ? null
            : playerInfo.m_characterID.ToString();
        var stableIdentifiers = new[]
        {
            playerInfo.m_userInfo.m_id.m_userID,
            playerInfo.m_userInfo.m_id.ToString(),
            player.GameId,
            player.PlatformId,
            player.SteamId
        };
        var names = new[]
        {
            playerInfo.m_name,
            playerInfo.m_serverAssignedDisplayName,
            playerInfo.m_userInfo.m_displayName,
            player.Name
        };
        if (PeerResolutionPolicy.TryAssociate(
                GetPeerCandidates(),
                characterId,
                stableIdentifiers,
                names,
                out var resolved,
                out _)
            && resolved is not null)
        {
            peer = resolved.Source;
            return true;
        }

        peer = null!;
        return false;
    }

    private PeerResolutionCandidate<ZNetPeer>[] GetPeerCandidates() =>
        (ZNet.instance?.GetPeers() ?? [])
            .Select(candidate => new PeerResolutionCandidate<ZNetPeer>(
                candidate,
                candidate.m_uid,
                candidate.IsReady(),
                candidate.m_characterID.IsNone() ? null : candidate.m_characterID.ToString(),
                candidate.m_socket?.GetHostName(),
                ToTakaroPlayer(candidate)))
            .ToArray();

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

}
#else
namespace Takaro.Valheim.Plugin;
#endif
