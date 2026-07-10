using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx.Logging;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

public sealed class ValheimServerAdapter : IValheimTakaroAdapter
{
    private readonly ManualLogSource logger;
    private readonly ConsoleCommandPolicy commandPolicy;
    private readonly PlayerPositionCache playerPositions = new(TimeSpan.FromSeconds(30));
    private readonly Dictionary<string, HashSet<string>> banAliases = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> banNames = new(StringComparer.OrdinalIgnoreCase);

    public ValheimServerAdapter(ManualLogSource logger, ConnectorConfig config)
    {
        this.logger = logger;
        commandPolicy = new ConsoleCommandPolicy(config.CommandAllowlistExact, config.CommandAllowlistPrefixes);
    }

    public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Ok(new { connectable = true }));

    public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default)
    {
        var network = ZNet.instance;
        playerPositions.SwitchWorld(network);
        if (network is null)
        {
            return Task.FromResult(RuntimeArrayActionPolicy.FromSource<TakaroPlayer>(
                sourceAvailable: false,
                values: null,
                sourceName: "Valheim networking"));
        }

        var players = network.GetPlayerList().Select(ToTakaroPlayer).ToArray();

        logger.LogInfo($"Takaro Valheim getPlayers returned {players.Length} player(s).");
        return Task.FromResult(RuntimeArrayActionPolicy.FromSource(
            sourceAvailable: true,
            values: players,
            sourceName: "Valheim player list"));
    }

    public async Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default)
    {
        var playersResult = await GetPlayersAsync(cancellationToken);
        return RuntimePlayerActionPolicy.Find(playersResult, identifier);
    }

    public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default)
    {
        var currentNetwork = ZNet.instance;
        playerPositions.SwitchWorld(currentNetwork);
        if (currentNetwork is null)
        {
            return Task.FromResult(TakaroActionResult.Error(
                "runtime_unavailable",
                "Valheim networking is not available yet."));
        }

        var now = DateTimeOffset.UtcNow;
        if (TryResolvePlayer(identifier, out var playerInfo, out var peer, out var player))
        {
            if (TryResolveServerKnownPosition(playerInfo, peer, out var position) && player is not null)
            {
                var observed = new TakaroPosition(position.x, position.y, position.z, "valheim");
                if (playerPositions.RememberIfCurrentWorld(currentNetwork, player, observed, now))
                {
                    return Task.FromResult(TakaroActionResult.Ok(observed));
                }
            }

            if (playerPositions.TryGetForCurrentWorld(currentNetwork, identifier, now, out var recent))
            {
                return Task.FromResult(TakaroActionResult.Ok(recent));
            }

            logger.LogInfo($"Takaro Valheim getPlayerLocation has no real server-observed position for '{identifier}'.");
            return Task.FromResult(TakaroActionResult.Error(
                "player_position_unavailable",
                $"Valheim does not expose a real server-observed position for player '{identifier}'."));
        }

        if (playerPositions.TryGetForCurrentWorld(currentNetwork, identifier, now, out var lastKnown))
        {
            logger.LogInfo($"Takaro Valheim getPlayerLocation returned a fresh server-observed last-known position for offline player '{identifier}'.");
            return Task.FromResult(TakaroActionResult.Ok(lastKnown));
        }

        return Task.FromResult(TakaroActionResult.Error(
            "player_not_found",
            $"Valheim player '{identifier}' is not online and has no fresh server-observed position."));
    }

    public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default)
    {
        logger.LogInfo($"Takaro Valheim getPlayerInventory is unsupported on a dedicated server for '{identifier}'.");
        return Task.FromResult(TakaroActionResult.Error(
            "player_component_unavailable",
            "Valheim dedicated servers do not expose remote player inventory components."));
    }

    public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default)
    {
        var amountValidation = GiveItemPolicy.PlanStacks(amount, maxStackSize: 0);
        if (!amountValidation.Success)
        {
            return Task.FromResult(TakaroActionResult.Error(
                amountValidation.ErrorCode!,
                amountValidation.ErrorMessage!));
        }

        if (ZRoutedRpc.instance is null)
        {
            return Task.FromResult(TakaroActionResult.Error("rpc_unavailable", "Valheim routed RPC is not available yet."));
        }

        if (!TryResolvePlayer(identifier, out var playerInfo, out var peer, out var player) || peer is null || player is null)
        {
            return Task.FromResult(TakaroActionResult.Error("player_not_found", $"Valheim player '{identifier}' is not online."));
        }

        if (!TryResolveServerKnownPosition(playerInfo, peer, out var position))
        {
            return Task.FromResult(TakaroActionResult.Error(
                "position_unavailable",
                $"Valheim has no server-known position for player '{identifier}'."));
        }

        if (!TryFindItemDropPrefab(itemCode, out var prefab, out var itemDrop))
        {
            return Task.FromResult(TakaroActionResult.Error("item_not_found", $"Valheim item '{itemCode}' was not found."));
        }

        if (!TryResolveQuality(quality, itemDrop, out var qualityLevel, out var qualityError))
        {
            return Task.FromResult(TakaroActionResult.Error("invalid_quality", qualityError!));
        }

        var maxStack = itemDrop.m_itemData.m_shared?.m_maxStackSize ?? 0;
        var stackPlan = GiveItemPolicy.PlanStacks(amount, maxStack);
        if (!stackPlan.Success)
        {
            return Task.FromResult(TakaroActionResult.Error(
                stackPlan.ErrorCode!,
                stackPlan.ErrorMessage!));
        }

        var dropCount = 0;
        foreach (var stack in stackPlan.Stacks)
        {
            var offset = new Vector3((dropCount % 3) - 1, 1.25f, dropCount / 3);
            DropItemStack(prefab, itemDrop, stack, qualityLevel, position + offset);
            dropCount++;
        }

        var itemName = DisplayName(itemDrop.m_itemData.m_shared?.m_name, prefab.name);
        SendHudMessage(peer, $"Dropped {amount}x {itemName} near you.");

        logger.LogInfo($"Takaro Valheim dropped {amount}x {prefab.name} for {player.Name} ({player.GameId}) at x={position.x}, y={position.y}, z={position.z}.");
        return Task.FromResult(TakaroActionResult.Ok(new
        {
            dropped = true,
            stacks = dropCount,
            player,
            item = new { code = prefab.name, name = itemName, amount, quality = qualityLevel.ToString() },
            position = new TakaroPosition(position.x, position.y, position.z, "valheim")
        }));
    }

    public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, CancellationToken cancellationToken = default)
    {
        if (ZRoutedRpc.instance is null)
        {
            return Task.FromResult(TakaroActionResult.Error("rpc_unavailable", "Valheim routed RPC is not available yet."));
        }

        if (!string.IsNullOrWhiteSpace(recipientIdentifier))
        {
            if (!TryResolvePlayer(recipientIdentifier!, out _, out var peer, out var recipient) || peer is null || recipient is null)
            {
                return Task.FromResult(TakaroActionResult.Error("player_not_found", $"Valheim player '{recipientIdentifier}' is not online."));
            }

            SendHudMessage(peer, message);
            logger.LogInfo($"Takaro Valheim server message routed to {recipient.Name} ({recipient.GameId}).");
            return Task.FromResult(TakaroActionResult.Ok(new { sent = true, recipient }));
        }

        var sent = 0;
        foreach (var peer in ZNet.instance?.GetPeers() ?? [])
        {
            if (!peer.IsReady())
            {
                continue;
            }

            SendHudMessage(peer, message);
            sent++;
        }

        logger.LogInfo($"Takaro Valheim server message routed to {sent} peer(s).");
        return Task.FromResult(TakaroActionResult.Ok(new { sent = sent > 0, recipients = sent }));
    }

    public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default)
    {
        logger.LogInfo($"Takaro command requested: {command}");
        ValheimChatEventBridge.EmitLog("info", $"Takaro command requested: {command}");
        if (!commandPolicy.IsAllowed(command))
        {
            logger.LogWarning($"Takaro Valheim blocked non-allowlisted console command: {command}");
            ValheimChatEventBridge.EmitLog("warning", $"Blocked non-allowlisted console command: {command}");
            return Task.FromResult(TakaroActionResult.Ok(new { success = false, rawResult = "command_not_allowed: Console command is not allowlisted." }));
        }

        if (Console.instance is not null)
        {
            Console.instance.TryRunCommand(command, silentFail: false, skipAllowedCheck: true);
        }
        else if (ZNet.instance is not null)
        {
            ZNet.instance.RemoteCommand(command);
        }
        else
        {
            return Task.FromResult(TakaroActionResult.Error("console_unavailable", "Valheim console command dispatcher is not available yet."));
        }

        logger.LogInfo($"Takaro Valheim executed allowlisted console command: {command}");
        ValheimChatEventBridge.EmitLog("info", $"Executed allowlisted console command: {command}");
        return Task.FromResult(TakaroActionResult.Ok(new { success = true, rawResult = $"Executed allowlisted Valheim console command: {command}" }));
    }

    public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default)
    {
        var scene = ZNetScene.instance;
        if (scene?.m_prefabs is null)
        {
            return Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(
                sourceAvailable: false,
                values: null,
                sourceName: "Valheim item prefab registry"));
        }

        var items = scene.m_prefabs
            .Select(prefab => new { Prefab = prefab, ItemDrop = prefab.GetComponent<ItemDrop>() })
            .Where(entry => entry.ItemDrop != null)
            .Select(entry => new
            {
                code = entry.Prefab.name,
                name = DisplayName(entry.ItemDrop.m_itemData.m_shared.m_name, entry.Prefab.name),
                amount = 1,
                quality = "1"
            })
            .GroupBy(item => item.code)
            .Select(group => group.First())
            .OrderBy(item => item.code)
            .ToArray();

        logger.LogInfo($"Takaro Valheim listItems returned {items.Length} item prefab(s).");
        return Task.FromResult(RuntimeArrayActionPolicy.FromSource(true, items, "Valheim item prefab registry"));
    }

    public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default)
    {
        var scene = ZNetScene.instance;
        if (scene?.m_prefabs is null)
        {
            return Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(
                sourceAvailable: false,
                values: null,
                sourceName: "Valheim entity prefab registry"));
        }

        var entities = scene.m_prefabs
            .Select(prefab => new { Prefab = prefab, Character = prefab.GetComponent<Character>() })
            .Where(entry => entry.Character != null && entry.Prefab.GetComponent<Player>() == null)
            .Select(entry => new
            {
                code = entry.Prefab.name,
                name = DisplayName(entry.Character.m_name, entry.Prefab.name)
            })
            .GroupBy(entity => entity.code)
            .Select(group => group.First())
            .OrderBy(entity => entity.code)
            .ToArray();

        logger.LogInfo($"Takaro Valheim listEntities returned {entities.Length} character prefab(s).");
        return Task.FromResult(RuntimeArrayActionPolicy.FromSource(true, entities, "Valheim entity prefab registry"));
    }

    public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default)
    {
        var zoneSystem = ZoneSystem.instance;
        if (zoneSystem is null)
        {
            return Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(
                sourceAvailable: false,
                values: null,
                sourceName: "Valheim zone system"));
        }

        var locations = zoneSystem.GetLocationList()
            .Select(instance => LocationFactory.Create(
                code: FirstNonEmpty(instance.m_location.m_prefabName, instance.m_location.m_name),
                rawName: instance.m_location.m_name,
                x: instance.m_position.x,
                y: instance.m_position.y,
                z: instance.m_position.z))
            .GroupBy(location => $"{location.Code}|{location.Position.X}|{location.Position.Y}|{location.Position.Z}", StringComparer.OrdinalIgnoreCase)
            .Select(group => group.First())
            .OrderBy(location => location.Code, StringComparer.OrdinalIgnoreCase)
            .ThenBy(location => location.Position.X)
            .ThenBy(location => location.Position.Z)
            .ToArray();

        logger.LogInfo($"Takaro Valheim listLocations returned {locations.Length} location(s).");
        return Task.FromResult(RuntimeArrayActionPolicy.FromSource(true, locations, "Valheim location registry"));
    }

    public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error(
            "server_only_unsupported",
            "Valheim dedicated servers do not expose the client map metadata required by getMapInfo."));

    public Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error(
            "server_only_unsupported",
            "Valheim dedicated servers do not expose client map tiles."));

    public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default)
    {
        if (ZRoutedRpc.instance is null)
        {
            return Task.FromResult(TakaroActionResult.Error("rpc_unavailable", "Valheim routed RPC is not available yet."));
        }

        if (!TryResolvePlayer(identifier, out _, out var peer, out var player) || peer is null || player is null)
        {
            return Task.FromResult(TakaroActionResult.Error("player_not_found", $"Valheim player '{identifier}' is not online."));
        }

        if (peer.m_characterID.IsNone())
        {
            return Task.FromResult(TakaroActionResult.Error(
                "character_unavailable",
                $"Valheim player '{identifier}' has no server-known character id."));
        }

        var target = new Vector3((float)position.X, (float)position.Y, (float)position.Z);
        ZRoutedRpc.instance.InvokeRoutedRPC(
            peer.m_uid,
            peer.m_characterID,
            "RPC_TeleportTo",
            target,
            Quaternion.identity,
            true);
        logger.LogInfo($"Takaro Valheim routed base-game teleportPlayer to {player.Name} ({player.GameId}): x={position.X}, y={position.Y}, z={position.Z}.");
        return Task.FromResult(TakaroActionResult.Ok(new { queued = true, player, position }));
    }

    public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default)
    {
        var znet = ZNet.instance;
        if (znet is null)
        {
            return Task.FromResult(TakaroActionResult.Error("znet_unavailable", "Valheim networking is not available yet."));
        }

        if (!TryResolvePlayer(identifier, out _, out var peer, out var player) || player is null)
        {
            return Task.FromResult(TakaroActionResult.Error("player_not_found", $"Valheim player '{identifier}' is not online."));
        }

        if (peer is not null)
        {
            peer.m_rpc?.Invoke("Kicked");
        }
        else
        {
            znet.Kick(player.GameId);
        }

        logger.LogInfo($"Takaro Valheim kicked {player.Name} ({player.GameId}). Reason: {reason ?? "<none>"}.");
        return Task.FromResult(TakaroActionResult.Ok());
    }

    public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default)
    {
        var znet = ZNet.instance;
        if (znet is null)
        {
            return Task.FromResult(TakaroActionResult.Error("znet_unavailable", "Valheim networking is not available yet."));
        }

        TryResolvePlayer(identifier, out _, out var peer, out var player);
        var primaryIdentifier = FirstNonEmpty(player?.GameId, identifier);
        var displayName = FirstNonEmpty(player?.Name, peer?.m_playerName, primaryIdentifier);

        znet.Ban(primaryIdentifier);
        RememberBanAliases(primaryIdentifier, displayName, identifier, player, peer);

        if (peer is not null)
        {
            peer.m_rpc?.Invoke("Kicked");
        }

        logger.LogInfo($"Takaro Valheim banned {displayName} ({primaryIdentifier}). Reason: {reason ?? "<none>"}.");
        return Task.FromResult(TakaroActionResult.Ok());
    }

    public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default)
    {
        var znet = ZNet.instance;
        if (znet is null)
        {
            return Task.FromResult(TakaroActionResult.Error("znet_unavailable", "Valheim networking is not available yet."));
        }

        var candidates = znet.Banned
            .Where(ban => BanMatchesIdentifier(ban, identifier))
            .Append(identifier)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        foreach (var candidate in candidates)
        {
            znet.Unban(candidate);
            banAliases.Remove(candidate);
            banNames.Remove(candidate);
        }

        logger.LogInfo($"Takaro Valheim unban requested for '{identifier}', removed {candidates.Length} matching alias(es).");
        return Task.FromResult(TakaroActionResult.Ok());
    }

    public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default)
    {
        var network = ZNet.instance;
        if (network?.Banned is null)
        {
            return Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(
                sourceAvailable: false,
                values: null,
                sourceName: "Valheim ban registry"));
        }

        var bans = network.Banned
            .Select(ban => new ValheimBan(
                GameId: ban,
                Name: banNames.TryGetValue(ban, out var name) ? name : ban,
                SteamId: ExtractSteamId(ban),
                PlatformId: ToPlatformId(ban)))
            .ToArray();

        logger.LogInfo($"Takaro Valheim listBans returned {bans.Length} official ban entry/entries.");
        return Task.FromResult(RuntimeArrayActionPolicy.FromSource(
            true,
            ModerationFactory.CreateBanEntries(bans),
            "Valheim ban registry"));
    }

    public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default)
    {
        logger.LogInfo("Takaro Valheim shutdown requested; scheduling Application.Quit after response flush.");
        _ = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(1));
            logger.LogInfo("Takaro Valheim executing scheduled shutdown.");
            Application.Quit();
        });

        return Task.FromResult(TakaroActionResult.Ok());
    }

    private TakaroPlayer ToTakaroPlayer(ZNet.PlayerInfo player)
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

    private bool TryFindPlayerInfo(string identifier, out ZNet.PlayerInfo player)
    {
        foreach (var candidate in ZNet.instance?.GetPlayerList() ?? [])
        {
            if (PlayerMapper.Find(new[] { ToTakaroPlayer(candidate) }, identifier) is not null)
            {
                player = candidate;
                return true;
            }
        }

        player = default;
        return false;
    }

    private bool TryResolvePlayer(string identifier, out ZNet.PlayerInfo playerInfo, out ZNetPeer? peer, out TakaroPlayer? player)
    {
        if (TryFindPlayerInfo(identifier, out playerInfo))
        {
            player = ToTakaroPlayer(playerInfo);
            peer = TryFindPeer(playerInfo, player, out var resolvedPeer) ? resolvedPeer : null;
            return true;
        }

        foreach (var candidate in ZNet.instance?.GetPeers() ?? [])
        {
            var candidatePlayer = ToTakaroPlayer(candidate);
            if (PlayerMapper.Find(new[] { candidatePlayer }, identifier) is not null)
            {
                playerInfo = default;
                peer = candidate;
                player = candidatePlayer;
                return true;
            }
        }

        playerInfo = default;
        peer = null;
        player = null;
        return false;
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

    private TakaroPlayer ToTakaroPlayer(ZNetPeer peer)
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

    private static void SendHudMessage(ZNetPeer peer, string message) =>
        SendClientMessage(peer, $"Takaro: {message}");

    private static void SendClientMessage(ZNetPeer peer, string message)
    {
        SendPlayerMessage(peer, MessageHud.MessageType.Center, message);
        SendPlayerMessage(peer, MessageHud.MessageType.TopLeft, message);

        ZRoutedRpc.instance.InvokeRoutedRPC(
            peer.m_uid,
            "ShowMessage",
            (int)MessageHud.MessageType.Center,
            message);

        ZRoutedRpc.instance.InvokeRoutedRPC(
            peer.m_uid,
            "ShowMessage",
            (int)MessageHud.MessageType.TopLeft,
            message);
    }

    private static void SendPlayerMessage(ZNetPeer peer, MessageHud.MessageType type, string message)
    {
        if (peer.m_characterID.IsNone())
        {
            return;
        }

        ZRoutedRpc.instance.InvokeRoutedRPC(
            peer.m_uid,
            peer.m_characterID,
            "Message",
            (int)type,
            message,
            0);
    }

    private void RememberBanAliases(string primaryIdentifier, string displayName, string requestedIdentifier, TakaroPlayer? player, ZNetPeer? peer)
    {
        var aliases = new[]
            {
                primaryIdentifier,
                requestedIdentifier,
                displayName,
                player?.GameId,
                player?.Name,
                player?.SteamId,
                player?.PlatformId,
                peer?.m_playerName,
                peer?.m_socket.GetHostName()
            }
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!)
            .ToArray();

        foreach (var alias in aliases)
        {
            if (!banAliases.TryGetValue(alias, out var values))
            {
                values = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                banAliases[alias] = values;
            }

            foreach (var value in aliases)
            {
                values.Add(value);
            }
        }

        banNames[primaryIdentifier] = displayName;
    }

    private bool BanMatchesIdentifier(string ban, string identifier)
    {
        if (Matches(ban, identifier))
        {
            return true;
        }

        if (banAliases.TryGetValue(identifier, out var aliases) && aliases.Contains(ban))
        {
            return true;
        }

        if (banAliases.TryGetValue(ban, out var banAliasSet) && banAliasSet.Contains(identifier))
        {
            return true;
        }

        return false;
    }

    private static bool TryResolveServerKnownPosition(ZNet.PlayerInfo playerInfo, ZNetPeer? peer, out Vector3 position)
    {
        if (peer is not null && peer.IsReady() && peer.m_refPos != Vector3.zero)
        {
            position = peer.m_refPos;
            return true;
        }

        if (playerInfo.m_publicPosition)
        {
            position = playerInfo.m_position;
            return true;
        }

        position = Vector3.zero;
        return false;
    }

    private static bool TryFindItemDropPrefab(string itemCode, out GameObject prefab, out ItemDrop itemDrop)
    {
        prefab = null!;
        itemDrop = null!;

        var scene = ZNetScene.instance;
        if (scene is null)
        {
            return false;
        }

        var candidates = new[]
            {
                scene.GetPrefab(itemCode),
                ObjectDB.instance?.GetItemPrefab(itemCode)
            }
            .Concat(scene.m_prefabs ?? [])
            .Where(candidate => candidate is not null)
            .Cast<GameObject>();

        foreach (var candidate in candidates)
        {
            if (candidate.TryGetComponent<ItemDrop>(out var candidateDrop)
                && ItemMatches(candidate, candidateDrop, itemCode))
            {
                prefab = candidate;
                itemDrop = candidateDrop;
                return true;
            }
        }

        return false;
    }

    private static bool ItemMatches(GameObject prefab, ItemDrop itemDrop, string itemCode)
    {
        var rawName = itemDrop.m_itemData.m_shared?.m_name;
        return Matches(prefab.name, itemCode)
            || Matches(DisplayName(rawName, prefab.name), itemCode)
            || Matches(rawName, itemCode);
    }

    private static bool TryResolveQuality(string? quality, ItemDrop itemDrop, out int qualityLevel, out string? error)
    {
        var maxQuality = itemDrop.m_itemData.m_shared?.m_maxQuality > 0
            ? itemDrop.m_itemData.m_shared.m_maxQuality
            : 1;
        if (string.IsNullOrWhiteSpace(quality))
        {
            qualityLevel = ClampQuality(itemDrop.m_itemData.m_quality, maxQuality);
            error = null;
            return true;
        }

        if (!int.TryParse(quality, out var parsed) || parsed <= 0)
        {
            qualityLevel = 1;
            error = $"Valheim item quality must be a positive integer, got '{quality}'.";
            return false;
        }

        qualityLevel = ClampQuality(parsed, maxQuality);
        error = null;
        return true;
    }

    private static int ClampQuality(int quality, int maxQuality) =>
        Math.Min(Math.Max(quality, 1), Math.Max(maxQuality, 1));

    private static void DropItemStack(
        GameObject prefab,
        ItemDrop itemDrop,
        int stack,
        int qualityLevel,
        Vector3 position)
    {
        var itemData = itemDrop.m_itemData.Clone();
        itemData.m_dropPrefab = prefab;
        itemData.m_quality = qualityLevel;
        itemData.m_stack = stack;
        ItemDrop.DropItem(itemData, stack, position, Quaternion.identity);
    }

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

    private static bool Matches(string? value, string? needle) =>
        !string.IsNullOrWhiteSpace(value)
        && !string.IsNullOrWhiteSpace(needle)
        && value!.Equals(needle, StringComparison.OrdinalIgnoreCase);

    private static string? ExtractSteamId(string value)
    {
        if (value.StartsWith("Steam_", StringComparison.OrdinalIgnoreCase))
        {
            return value.Substring("Steam_".Length);
        }

        return value.All(char.IsDigit) && value.Length == 17 ? value : null;
    }

    private static string? ToPlatformId(string value)
    {
        var steamId = ExtractSteamId(value);
        return string.IsNullOrWhiteSpace(steamId) ? null : $"steam:{steamId}";
    }

    private static string DisplayName(string? rawName, string fallback)
    {
        if (string.IsNullOrWhiteSpace(rawName))
        {
            return fallback;
        }

        var displayName = rawName!.Trim().Trim('$');
        return string.IsNullOrWhiteSpace(displayName) ? fallback : displayName;
    }
}
#else
namespace Takaro.Valheim.Plugin;

public sealed class ValheimServerAdapter : IValheimTakaroAdapter
{
    public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Ok(new { connectable = false }));

    public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(RuntimeArrayActionPolicy.FromSource<TakaroPlayer>(false, null, "Valheim networking"));

    public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("runtime_unavailable", "Valheim networking is unavailable in reference-free scaffold mode."));

    public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error(
            "player_position_unavailable",
            "Reference-free scaffold has no server-owned player position."));

    public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error(
            "player_component_unavailable",
            "Reference-free scaffold has no server-owned Player inventory component."));

    public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable item giving."));

    public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable server messaging."));

    public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable console commands."));

    public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(false, null, "Valheim item prefab registry"));

    public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(false, null, "Valheim entity prefab registry"));

    public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(false, null, "Valheim zone system"));

    public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("server_only_unsupported", "Valheim dedicated servers do not expose client map metadata."));

    public Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("server_only_unsupported", "Valheim dedicated servers do not expose client map tiles."));

    public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable teleport."));

    public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable moderation."));

    public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable moderation."));

    public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable moderation."));

    public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(RuntimeArrayActionPolicy.FromSource<object>(false, null, "Valheim ban registry"));

    public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Error("scaffold_mode", "Build with Valheim references to enable shutdown."));
}
#endif
