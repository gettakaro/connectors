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
    private static readonly Dictionary<int, int> RoutedRpcDiagnostics = new();
    private static readonly InventorySnapshotCache InventorySnapshots = new();
    private static readonly LocationSnapshotCache LocationSnapshots = new();
    private static readonly System.Reflection.FieldInfo? LastHitField = AccessTools.Field(typeof(Character), "m_lastHit");
    private static int routedDiagnosticsRemaining = 5000;
    private static ZRoutedRpc? registeredRpc;
    private static TakaroWebSocketRunner? runner;
    private static Action<string> log = _ => { };
    private static DateTimeOffset nextInventorySnapshotAt = DateTimeOffset.MinValue;
    private static DateTimeOffset nextLocationSnapshotAt = DateTimeOffset.MinValue;

    public static void Initialize(TakaroWebSocketRunner? activeRunner, Action<string>? logger)
    {
        runner = activeRunner;
        log = logger ?? (_ => { });
        log($"Takaro Valheim chat hash diagnostics: ChatMessage={ChatMessageHash}, Say={SayHash}.");
        log($"Takaro Valheim chat hash candidates: RPC_ChatMessage={"RPC_ChatMessage".GetStableHashCode()}, RPC_Say={"RPC_Say".GetStableHashCode()}, SendText={"SendText".GetStableHashCode()}, ChatMessageToAll={"ChatMessageToAll".GetStableHashCode()}, NewChatMessage={"NewChatMessage".GetStableHashCode()}.");
    }

    public static void Shutdown()
    {
        runner = null;
        registeredRpc = null;
        lock (Sync)
        {
            RecentEvents.Clear();
            RecentEntityDeaths.Clear();
            RoutedRpcDiagnostics.Clear();
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
        if (IsDedicatedServer())
        {
            routedRpc.Register<int, string>("TakaroClientChatMessage", RPC_TakaroClientChatMessage);
            routedRpc.Register<string>("TakaroClientInventorySnapshot", RPC_TakaroClientInventorySnapshot);
            routedRpc.Register<string>("TakaroClientLocationSnapshot", RPC_TakaroClientLocationSnapshot);
            routedRpc.Register<string>("TakaroPlayerDeath", RPC_TakaroPlayerDeath);
            routedRpc.Register<string>("TakaroEntityKilled", RPC_TakaroEntityKilled);
            log("Takaro Valheim registered server chat, inventory, location, death, and entity-kill bridge RPCs.");
        }
        else
        {
            routedRpc.Register<string>("TakaroServerMessage", RPC_TakaroServerMessage);
            routedRpc.Register<string, int, string>("TakaroGiveItem", RPC_TakaroGiveItem);
            routedRpc.Register<float, float, float>("TakaroTeleportPlayer", RPC_TakaroTeleportPlayer);
            log("Takaro Valheim registered client message bridge RPC.");
        }

        if (!IsDedicatedServer())
        {
            TrySendLocalInventorySnapshot(force: true);
            TrySendLocalLocationSnapshot(force: true);
        }
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

        _ = SendAsync(activeRunner, evt);
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

    public static void ForwardLocalChat(Talker.Type type, string text)
    {
        if (IsDedicatedServer() || string.IsNullOrWhiteSpace(text) || ZRoutedRpc.instance is null)
        {
            return;
        }

        try
        {
            ZRoutedRpc.instance.InvokeRoutedRPC("TakaroClientChatMessage", (int)type, text);
            log($"Takaro Valheim forwarded local chat to server: channel={ChannelName((int)type)}, msgLength={text.Length}.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim local chat forward failed: {ex.Message}");
        }
    }

    public static bool TryGetInventorySnapshot(string identifier, out IReadOnlyList<TakaroInventoryItem> items) =>
        InventorySnapshots.TryGet(identifier, out items);

    public static bool TryGetLocationSnapshot(string identifier, out TakaroPosition position) =>
        LocationSnapshots.TryGet(identifier, out position);

    public static void ObserveRoutedRpc(ZPackage package)
    {
        var originalPosition = package.GetPos();
        try
        {
            var data = new ZRoutedRpc.RoutedRPCData();
            data.Deserialize(package);
            ObserveRoutedRpcData(data, "RPC_RoutedRPC");
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

    public static void ObserveRoutedRpcData(ZRoutedRpc.RoutedRPCData data, string source)
    {
        var originalPosition = data.m_parameters.GetPos();
        try
        {
            data.m_parameters.SetPos(0);

            if (data.m_methodHash == ChatMessageHash)
            {
                log($"Takaro Valheim observed routed ChatMessage packet at {source}.");
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
                log($"Takaro Valheim observed routed Say packet at {source}.");
                var chatType = data.m_parameters.ReadInt();
                var userInfo = new UserInfo();
                userInfo.Deserialize(ref data.m_parameters);
                var text = data.m_parameters.ReadString();
                Emit(data.m_senderPeerID, chatType, userInfo, text);
                return;
            }

            if (ShouldLogRoutedRpc(data.m_methodHash))
            {
                log($"Takaro Valheim observed routed RPC at {source}: hash={data.m_methodHash}, sender={data.m_senderPeerID}, targetPeer={data.m_targetPeerID}, targetZdo={data.m_targetZDO}, payload={DescribePackage(data.m_parameters)}.");
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not inspect routed chat data at {source}: {ex.Message}");
        }
        finally
        {
            data.m_parameters.SetPos(originalPosition);
        }
    }

    private static void RPC_TakaroClientChatMessage(long sender, int chatType, string text)
    {
        if (!IsDedicatedServer())
        {
            return;
        }

        var userInfo = ResolveUserInfo(sender);
        log($"Takaro Valheim received bridged client chat: sender={sender}, player={userInfo.Name}, channel={ChannelName(chatType)}, msgLength={text.Length}.");
        Emit(sender, chatType, userInfo, text);
    }

    private static void RPC_TakaroClientInventorySnapshot(long sender, string inventoryJson)
    {
        if (!IsDedicatedServer())
        {
            return;
        }

        try
        {
            var items = JsonSerializer.Deserialize<TakaroInventoryItem[]>(inventoryJson) ?? [];
            var userInfo = ResolveUserInfo(sender);
            var player = PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
                FirstNonEmpty(userInfo.Name, userInfo.GetDisplayName(), sender.ToString()),
                FirstNonEmpty(userInfo.UserId.ToString(), sender.ToString()),
                null,
                null,
                null));

            if (InventorySnapshots.Store(player, items, DateTimeOffset.UtcNow))
            {
                log($"Takaro Valheim cached inventory snapshot for {player.Name} ({player.GameId}): {items.Length} stack(s).");
            }
            else
            {
                log($"Takaro Valheim ignored transient empty inventory snapshot for {player.Name} ({player.GameId}).");
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim inventory snapshot receive failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroClientLocationSnapshot(long sender, string locationJson)
    {
        if (!IsDedicatedServer())
        {
            return;
        }

        try
        {
            var position = JsonSerializer.Deserialize<TakaroPosition>(locationJson) ?? new TakaroPosition(0, 0, 0, "valheim");
            var player = PlayerFromSender(sender);

            if (LocationSnapshots.Store(player, position, DateTimeOffset.UtcNow))
            {
                log($"Takaro Valheim cached location snapshot for {player.Name} ({player.GameId}): x={position.X}, y={position.Y}, z={position.Z}.");
            }
            else
            {
                log($"Takaro Valheim ignored older location snapshot for {player.Name} ({player.GameId}).");
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim location snapshot receive failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroPlayerDeath(long sender, string deathJson)
    {
        if (!IsDedicatedServer())
        {
            return;
        }

        try
        {
            var death = JsonSerializer.Deserialize<ClientDeathSnapshot>(deathJson);
            if (death is null)
            {
                return;
            }

            var activeRunner = runner;
            if (activeRunner is null)
            {
                return;
            }

            var player = PlayerFromSender(sender);
            var attacker = FindTakaroPlayer(FirstNonEmptyOrNull(death.AttackerGameId, death.AttackerName));
            var evt = EventFactory.PlayerDeath(
                player,
                DateTimeOffset.UtcNow,
                death.Position ?? new TakaroPosition(0, 0, 0, "valheim"),
                attacker,
                death.Weapon);

            _ = SendGameEventAsync(
                activeRunner,
                "player-death",
                evt,
                $"Takaro Valheim player-death event sent for {player.Name} ({player.GameId}).",
                "Takaro Valheim player-death event send failed");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim player death receive failed: {ex.Message}");
            EmitLog("error", $"Player death receive failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroEntityKilled(long sender, string entityKilledJson)
    {
        if (!IsDedicatedServer())
        {
            return;
        }

        try
        {
            var snapshot = JsonSerializer.Deserialize<ClientEntityKilledSnapshot>(entityKilledJson);
            if (snapshot?.Entity is null)
            {
                return;
            }

            var activeRunner = runner;
            if (activeRunner is null)
            {
                return;
            }

            var position = snapshot.Position ?? new TakaroPosition(0, 0, 0, "valheim");
            if (!ShouldEmitEntityDeath($"{sender}|{snapshot.Entity.Code}|{position.X:F1}|{position.Y:F1}|{position.Z:F1}"))
            {
                return;
            }

            var killer = PlayerFromSender(sender);
            var evt = EventFactory.EntityKilled(
                snapshot.Entity,
                DateTimeOffset.UtcNow,
                position,
                killer,
                snapshot.Weapon);

            _ = SendGameEventAsync(
                activeRunner,
                "entity-killed",
                evt,
                $"Takaro Valheim entity-killed event sent for {snapshot.Entity.Code}.",
                "Takaro Valheim entity-killed event send failed");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim entity-killed receive failed: {ex.Message}");
            EmitLog("error", $"Entity-killed receive failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroServerMessage(long sender, string text)
    {
        try
        {
            Chat.instance?.AddString("Takaro", text, Talker.Type.Normal);
            MessageHud.instance?.ShowMessage(MessageHud.MessageType.Center, $"Takaro: {text}");
            MessageHud.instance?.ShowMessage(MessageHud.MessageType.TopLeft, $"Takaro: {text}");
            log($"Takaro Valheim displayed server message from bridge: sender={sender}, msgLength={text.Length}.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim client message display failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroGiveItem(long sender, string itemCode, int amount, string quality)
    {
        try
        {
            var player = Player.m_localPlayer;
            if (player is null)
            {
                log($"Takaro Valheim giveItem failed: no local player. sender={sender}, item={itemCode}, amount={amount}.");
                return;
            }

            var itemPrefab = ObjectDB.instance?.GetItemPrefab(itemCode);
            if (itemPrefab is null)
            {
                log($"Takaro Valheim giveItem failed: item not found. sender={sender}, item={itemCode}, amount={amount}.");
                player.Message(MessageHud.MessageType.Center, $"Takaro item not found: {itemCode}");
                return;
            }

            var qualityLevel = ParseQuality(quality);
            var added = player.GetInventory().AddItem(itemCode, Math.Max(1, amount), qualityLevel, 0, 0L, "Takaro", pickedUp: true);
            if (added is null)
            {
                log($"Takaro Valheim giveItem failed: inventory full. sender={sender}, item={itemCode}, amount={amount}.");
                player.Message(MessageHud.MessageType.Center, $"Takaro could not give {itemCode}: inventory full");
                return;
            }

            player.Message(MessageHud.MessageType.TopLeft, $"Takaro gave {amount} x {DisplayItemName(itemPrefab, itemCode)}");
            log($"Takaro Valheim giveItem applied locally: sender={sender}, item={itemCode}, amount={amount}, quality={qualityLevel}.");
            TrySendLocalInventorySnapshot(force: true);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim giveItem client RPC failed: {ex.Message}");
        }
    }

    private static void RPC_TakaroTeleportPlayer(long sender, float x, float y, float z)
    {
        try
        {
            var player = Player.m_localPlayer;
            if (player is null)
            {
                log($"Takaro Valheim teleportPlayer failed: no local player. sender={sender}, x={x}, y={y}, z={z}.");
                return;
            }

            var queued = player.TeleportTo(new Vector3(x, y, z), Quaternion.identity, distantTeleport: true);
            player.Message(MessageHud.MessageType.TopLeft, queued ? "Takaro teleport queued" : "Takaro teleport could not start");
            log($"Takaro Valheim teleportPlayer applied locally: sender={sender}, x={x}, y={y}, z={z}, queued={queued}.");
            TrySendLocalLocationSnapshot(force: true);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim teleportPlayer client RPC failed: {ex.Message}");
        }
    }

    private static async Task SendAsync(TakaroWebSocketRunner activeRunner, object evt)
    {
        await SendGameEventAsync(
            activeRunner,
            "chat-message",
            evt,
            "Takaro Valheim chat event sent to Takaro.",
            "Takaro Valheim chat event send failed");
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

    internal static void TrySendLocalInventorySnapshot(bool force = false)
    {
        if (IsDedicatedServer() || ZRoutedRpc.instance is null)
        {
            return;
        }

        var now = DateTimeOffset.UtcNow;
        if (!force && now < nextInventorySnapshotAt)
        {
            return;
        }

        nextInventorySnapshotAt = now.AddSeconds(10);

        try
        {
            var player = Player.m_localPlayer;
            if (player is null)
            {
                return;
            }

            var items = player.GetInventory().GetAllItems()
                .Select(item => new TakaroInventoryItem(
                    Code: item.m_dropPrefab != null ? item.m_dropPrefab.name : item.m_shared.m_name,
                    Name: DisplayName(item.m_shared.m_name, item.m_dropPrefab != null ? item.m_dropPrefab.name : item.m_shared.m_name),
                    Amount: item.m_stack,
                    Quality: item.m_quality.ToString(),
                    Durability: item.m_durability,
                    Equipped: item.m_equipped,
                    Position: new TakaroInventorySlot(item.m_gridPos.x, item.m_gridPos.y)))
                .ToArray();

            ZRoutedRpc.instance.InvokeRoutedRPC("TakaroClientInventorySnapshot", JsonSerializer.Serialize(items));
            log($"Takaro Valheim sent local inventory snapshot: {items.Length} stack(s).");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim local inventory snapshot failed: {ex.Message}");
        }
    }

    internal static void TrySendLocalLocationSnapshot(bool force = false)
    {
        if (IsDedicatedServer() || ZRoutedRpc.instance is null)
        {
            return;
        }

        var now = DateTimeOffset.UtcNow;
        if (!force && now < nextLocationSnapshotAt)
        {
            return;
        }

        nextLocationSnapshotAt = now.AddSeconds(2);

        try
        {
            var player = Player.m_localPlayer;
            if (player is null)
            {
                return;
            }

            var position = player.transform.position;
            var snapshot = new TakaroPosition(position.x, position.y, position.z, "valheim");
            ZRoutedRpc.instance.InvokeRoutedRPC("TakaroClientLocationSnapshot", JsonSerializer.Serialize(snapshot));
            log($"Takaro Valheim sent local location snapshot: x={snapshot.X}, y={snapshot.Y}, z={snapshot.Z}.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim local location snapshot failed: {ex.Message}");
        }
    }

    internal static void ForwardLocalPlayerDeath(Player player)
    {
        if (IsDedicatedServer() || ZRoutedRpc.instance is null || Player.m_localPlayer != player)
        {
            return;
        }

        try
        {
            var position = player.transform.position;
            var hit = GetLastHit(player);
            var attacker = hit?.GetAttacker();
            var attackerPlayer = attacker as Player;
            var snapshot = new ClientDeathSnapshot(
                new TakaroPosition(position.x, position.y, position.z, "valheim"),
                attacker?.GetHoverName(),
                attackerPlayer?.GetZDOID().UserID.ToString(),
                hit is null ? null : hit.m_skill.ToString());

            ZRoutedRpc.instance.InvokeRoutedRPC("TakaroPlayerDeath", JsonSerializer.Serialize(snapshot));
            log($"Takaro Valheim forwarded local player death: x={position.x}, y={position.y}, z={position.z}, attacker={snapshot.AttackerName ?? "<none>"}.");
            TrySendLocalLocationSnapshot(force: true);
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim local player death forward failed: {ex.Message}");
        }
    }

    internal static void ForwardLocalEntityKilled(Character character)
    {
        if (IsDedicatedServer()
            || ZRoutedRpc.instance is null
            || character is Player
            || character.GetComponent<Player>() != null
            || !ShouldEmitEntityDeath(character))
        {
            return;
        }

        try
        {
            var localPlayer = Player.m_localPlayer;
            var hit = GetLastHit(character);
            var attacker = hit?.GetAttacker();
            if (localPlayer is null || attacker != localPlayer)
            {
                return;
            }

            var position = character.transform.position;
            var entity = new TakaroEntity(
                string.IsNullOrWhiteSpace(character.name) ? character.GetHoverName() : character.name,
                DisplayName(character.m_name, character.GetHoverName()));
            var snapshot = new ClientEntityKilledSnapshot(
                entity,
                new TakaroPosition(position.x, position.y, position.z, "valheim"),
                hit is null ? null : hit.m_skill.ToString());

            ZRoutedRpc.instance.InvokeRoutedRPC("TakaroEntityKilled", JsonSerializer.Serialize(snapshot));
            log($"Takaro Valheim forwarded local entity kill: entity={entity.Code}, x={position.x}, y={position.y}, z={position.z}.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim local entity kill forward failed: {ex.Message}");
        }
    }

    internal static void EmitEntityKilled(Character character)
    {
        if (!IsDedicatedServer() || character is Player || character.GetComponent<Player>() != null || !ShouldEmitEntityDeath(character))
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

    private static UserInfo ResolveUserInfo(long sender)
    {
        var peer = ZNet.instance?.GetPeer(sender);
        var playerInfo = ZNet.instance?.GetPlayerList()
            .FirstOrDefault(player => player.m_characterID.UserID == sender || player.m_name == peer?.m_playerName);

        if (playerInfo is { } info && !string.IsNullOrWhiteSpace(info.m_name))
        {
            return new UserInfo
            {
                Name = FirstNonEmpty(info.m_name, info.m_serverAssignedDisplayName, info.m_userInfo.m_displayName, sender.ToString()),
                UserId = info.m_userInfo.m_id
            };
        }

        return new UserInfo
        {
            Name = FirstNonEmpty(peer?.m_playerName, sender.ToString()),
            UserId = default
        };
    }

    private static TakaroPlayer PlayerFromSender(long sender)
    {
        var userInfo = ResolveUserInfo(sender);
        return PlayerMapper.ToTakaroPlayer(new ValheimPlayer(
            FirstNonEmpty(userInfo.Name, userInfo.GetDisplayName(), sender.ToString()),
            FirstNonEmpty(userInfo.UserId.ToString(), sender.ToString()),
            null,
            null,
            null));
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

    private static bool ShouldLogRoutedRpc(int methodHash)
    {
        if (routedDiagnosticsRemaining <= 0)
        {
            return false;
        }

        lock (Sync)
        {
            routedDiagnosticsRemaining--;
            RoutedRpcDiagnostics.TryGetValue(methodHash, out var count);
            count++;
            RoutedRpcDiagnostics[methodHash] = count;
            return count <= 20 || count % 100 == 0;
        }
    }

    private static string DescribePackage(ZPackage package)
    {
        var originalPosition = package.GetPos();
        try
        {
            var bytes = package.GetArray() ?? [];
            var hex = BitConverter.ToString(bytes.Take(96).ToArray()).Replace("-", string.Empty);
            var ascii = new string(bytes.Take(160).Select(value => value >= 32 && value <= 126 ? (char)value : '.').ToArray());
            var strings = ExtractPrintableStrings(bytes, 3).Take(8).ToArray();
            return $"size={package.Size()}, pos={originalPosition}, hex96={hex}, ascii={ascii}, strings=[{string.Join("|", strings)}]";
        }
        catch (Exception ex)
        {
            return $"unavailable:{ex.Message}";
        }
        finally
        {
            package.SetPos(originalPosition);
        }
    }

    private static IEnumerable<string> ExtractPrintableStrings(byte[] bytes, int minLength)
    {
        var buffer = new List<char>();
        foreach (var value in bytes)
        {
            if (value >= 32 && value <= 126)
            {
                buffer.Add((char)value);
                continue;
            }

            if (buffer.Count >= minLength)
            {
                yield return new string(buffer.ToArray());
            }

            buffer.Clear();
        }

        if (buffer.Count >= minLength)
        {
            yield return new string(buffer.ToArray());
        }
    }

    public static void LogChatPatchHit(string source, long sender, int chatType, UserInfo userInfo, string text, bool dedicated)
    {
        log($"Takaro Valheim chat patch hit: source={source}, dedicated={dedicated}, sender={sender}, userName={FirstNonEmpty(userInfo.Name, userInfo.GetDisplayName(), "<empty>")}, userId={userInfo.UserId}, channel={ChannelName(chatType)}, msgLength={(text ?? string.Empty).Length}.");
    }

    public static void LogLocalSendTextPatchHit(Talker.Type type, string text)
    {
        log($"Takaro Valheim Chat.SendText postfix hit: dedicated={IsDedicatedServer()}, channel={ChannelName((int)type)}, msgLength={(text ?? string.Empty).Length}.");
    }

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "unknown";

    private static string? FirstNonEmptyOrNull(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value));

    private static bool Matches(string? value, string? needle) =>
        !string.IsNullOrWhiteSpace(value)
        && !string.IsNullOrWhiteSpace(needle)
        && value!.Equals(needle, StringComparison.OrdinalIgnoreCase);

    private static int ParseQuality(string quality) =>
        int.TryParse(quality, out var parsed) && parsed > 0 ? parsed : 1;

    private static string DisplayItemName(GameObject itemPrefab, string fallback)
    {
        var itemDrop = itemPrefab.GetComponent<ItemDrop>();
        if (itemDrop is null)
        {
            return fallback;
        }

        var rawName = itemDrop.m_itemData.m_shared.m_name;
        var displayName = string.IsNullOrWhiteSpace(rawName) ? fallback : rawName.Trim().Trim('$');
        return string.IsNullOrWhiteSpace(displayName) ? fallback : displayName;
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

    private sealed class ClientDeathSnapshot
    {
        public ClientDeathSnapshot(
            TakaroPosition? position,
            string? attackerName,
            string? attackerGameId,
            string? weapon)
        {
            Position = position;
            AttackerName = attackerName;
            AttackerGameId = attackerGameId;
            Weapon = weapon;
        }

        public TakaroPosition? Position { get; }

        public string? AttackerName { get; }

        public string? AttackerGameId { get; }

        public string? Weapon { get; }
    }

    private sealed class ClientEntityKilledSnapshot
    {
        public ClientEntityKilledSnapshot(TakaroEntity? entity, TakaroPosition? position, string? weapon)
        {
            Entity = entity;
            Position = position;
            Weapon = weapon;
        }

        public TakaroEntity? Entity { get; }

        public TakaroPosition? Position { get; }

        public string? Weapon { get; }
    }
}

[HarmonyPatch(typeof(Chat), "RPC_ChatMessage")]
internal static class TakaroChatRpcChatMessagePatch
{
    private static bool Prefix(long sender, Vector3 position, int type, UserInfo userInfo, string text)
    {
        ValheimChatEventBridge.LogChatPatchHit("Chat.RPC_ChatMessage", sender, type, userInfo, text, IsDedicatedServer());
        ValheimChatEventBridge.Emit(sender, type, userInfo, text);
        return !IsDedicatedServer();
    }

    private static bool IsDedicatedServer() =>
        ZNet.instance is not null && ZNet.instance.IsDedicated();
}

[HarmonyPatch(typeof(Talker), "RPC_Say")]
internal static class TakaroTalkerRpcSayPatch
{
    private static bool Prefix(long sender, int ctype, UserInfo user, string text)
    {
        ValheimChatEventBridge.LogChatPatchHit("Talker.RPC_Say", sender, ctype, user, text, IsDedicatedServer());
        ValheimChatEventBridge.Emit(sender, ctype, user, text);
        return !IsDedicatedServer();
    }

    private static bool IsDedicatedServer() =>
        ZNet.instance is not null && ZNet.instance.IsDedicated();
}

[HarmonyPatch(typeof(ZRoutedRpc), "RPC_RoutedRPC")]
internal static class TakaroRoutedRpcPatch
{
    private static void Prefix(ZRpc rpc, ZPackage pkg)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimChatEventBridge.ObserveRoutedRpc(pkg);
        }
    }
}

[HarmonyPatch(typeof(ZRoutedRpc), "RouteRPC")]
internal static class TakaroRoutedRpcRoutePatch
{
    private static void Prefix(ZRoutedRpc.RoutedRPCData rpcData)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimChatEventBridge.ObserveRoutedRpcData(rpcData, "RouteRPC");
        }
    }
}

[HarmonyPatch(typeof(ZRoutedRpc), "HandleRoutedRPC")]
internal static class TakaroRoutedRpcHandlePatch
{
    private static void Prefix(ZRoutedRpc.RoutedRPCData data)
    {
        if (ZNet.instance is not null && ZNet.instance.IsDedicated())
        {
            ValheimChatEventBridge.ObserveRoutedRpcData(data, "HandleRoutedRPC");
        }
    }
}

[HarmonyPatch(typeof(Chat), "SendText")]
internal static class TakaroChatSendTextPatch
{
    private static void Postfix(Talker.Type type, string text)
    {
        ValheimChatEventBridge.LogLocalSendTextPatchHit(type, text);
        ValheimChatEventBridge.ForwardLocalChat(type, text);
    }
}

[HarmonyPatch(typeof(Player), "Update")]
internal static class TakaroPlayerUpdatePatch
{
    private static void Postfix(Player __instance)
    {
        if (Player.m_localPlayer == __instance)
        {
            ValheimChatEventBridge.TrySendLocalInventorySnapshot();
            ValheimChatEventBridge.TrySendLocalLocationSnapshot();
        }
    }
}

[HarmonyPatch(typeof(Player), "OnDeath")]
internal static class TakaroPlayerOnDeathPatch
{
    private static void Postfix(Player __instance) =>
        ValheimChatEventBridge.ForwardLocalPlayerDeath(__instance);
}

[HarmonyPatch(typeof(Character), "OnDeath")]
internal static class TakaroCharacterOnDeathPatch
{
    private static void Postfix(Character __instance)
    {
        ValheimChatEventBridge.EmitEntityKilled(__instance);
        ValheimChatEventBridge.ForwardLocalEntityKilled(__instance);
    }
}
#else
namespace Takaro.Valheim.Plugin;
#endif
