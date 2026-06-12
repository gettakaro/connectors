using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Takaro.Services;
using UnityEngine;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Action requests mutate game state, so their bodies run on the game main
    /// thread via the MainThreadDispatcher and are awaited from the WebSocket
    /// thread. Exceptions propagate to the RequestRouter error boundary.
    /// </summary>
    public static class ActionHandlers
    {
        public static async Task GiveItem(string requestId, TakaroGiveItemArgs args)
        {
            if (args == null || args.GameId == null || string.IsNullOrEmpty(args.Item))
            {
                SendError(requestId, "Invalid or missing parameters");
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.GameId);
                if (cInfo == null)
                {
                    SendError(requestId, "Player not found");
                    return;
                }

                ItemValue itemValue = ItemClass.GetItem(args.Item);
                if (itemValue == null || itemValue.type == ItemValue.None.type)
                {
                    SendError(requestId, "Item not found");
                    return;
                }

                if (
                    !GameManager.Instance.World.Players.dict.TryGetValue(
                        cInfo.entityId,
                        out EntityPlayer player
                    )
                )
                {
                    SendError(requestId, "Player entity not found");
                    return;
                }

                if (!player.IsSpawned())
                {
                    SendError(requestId, "Player is not spawned");
                    return;
                }

                if (player.IsDead())
                {
                    SendError(requestId, "Player is dead");
                    return;
                }

                if (args.Amount <= 0)
                {
                    SendError(requestId, "Invalid item amount");
                    return;
                }

                ushort quality = Constants.cItemMaxQuality;
                if (!string.IsNullOrEmpty(args.Quality))
                {
                    if (
                        ushort.TryParse(args.Quality, out ushort parsedQuality)
                        && parsedQuality <= Constants.cItemMaxQuality
                    )
                    {
                        quality = parsedQuality;
                    }
                    else
                    {
                        SendError(requestId, "Invalid quality value");
                        return;
                    }
                }

                ItemValue iv = new ItemValue(itemValue.type, true);

                if (ItemClass.list[iv.type].HasSubItems)
                {
                    for (int i = 0; i < iv.Modifications.Length; i++)
                    {
                        ItemValue tmp = iv.Modifications[i];
                        tmp.Quality = quality;
                        iv.Modifications[i] = tmp;
                    }
                }
                else if (ItemClass.list[iv.type].HasQuality)
                {
                    iv.Quality = quality;
                }

                ItemStack itemStack = new ItemStack(iv, args.Amount);
                World world = GameManager.Instance.World;
                EntityItem entityItem = (EntityItem)
                    EntityFactory.CreateEntity(
                        new EntityCreationData
                        {
                            entityClass = EntityClass.FromString("item"),
                            id = EntityFactory.nextEntityID++,
                            itemStack = itemStack,
                            pos = world.Players.dict[cInfo.entityId].position,
                            rot = new Vector3(20f, 0f, 20f),
                            lifetime = 60f,
                            belongsPlayerId = cInfo.entityId,
                        }
                    );
                world.SpawnEntityInWorld(entityItem);
                cInfo.SendPackage(
                    NetPackageManager
                        .GetPackage<NetPackageEntityCollect>()
                        .Setup(entityItem.entityId, cInfo.entityId)
                );
                world.RemoveEntity(entityItem.entityId, EnumRemoveEntityReason.Despawned);

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object> { },
                        requestId
                    )
                );
            });
        }

        public static async Task SendChatMessage(string requestId, TakaroSendMessageArgs args)
        {
            if (args == null || string.IsNullOrEmpty(args.Message))
            {
                SendError(requestId, "Invalid or missing parameters");
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                // If a GameId is provided, send the message to that player as a whisper
                if (args.Recipient != null && args.Recipient.GameId != null)
                {
                    ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Recipient.GameId);
                    if (cInfo == null)
                    {
                        SendError(requestId, "Player not found");
                        return;
                    }

                    cInfo.SendPackage(
                        NetPackageManager
                            .GetPackage<NetPackageChat>()
                            .Setup(
                                EChatType.Whisper,
                                -1,
                                args.Message,
                                null,
                                EMessageSender.Server,
                                GeneratedTextManager.BbCodeSupportMode.Supported
                            )
                    );
                }
                else
                {
                    GameManager.Instance.ChatMessageServer(
                        null,
                        EChatType.Global,
                        -1,
                        args.Message,
                        null,
                        EMessageSender.Server
                    );
                }

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object> { },
                        requestId
                    )
                );
            });
        }

        public static async Task ExecuteCommand(string requestId, TakaroExecuteCommandArgs args)
        {
            if (args == null || string.IsNullOrEmpty(args.Command))
            {
                SendError(requestId, "Invalid or missing command");
                return;
            }

            var tcs = new TaskCompletionSource<string>();
            var cr = new CommandResult(args.Command, tcs);
            SdtdConsole.Instance.ExecuteAsync(args.Command, cr);
            string result = await tcs.Task;

            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> { { "rawResult", result }, { "success", true } },
                    requestId
                )
            );
        }

        public static async Task KickPlayer(string requestId, TakaroKickPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendError(requestId, "Invalid or missing gameId parameter");
                return;
            }

            string kickReason = string.IsNullOrEmpty(args.Reason) ? "Kicked by admin" : args.Reason;

            await MainThreadDispatcher.Instance.Run(() =>
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
                if (cInfo == null)
                {
                    SendError(requestId, "Player not found");
                    return;
                }

                var kickData = new GameUtils.KickPlayerData(
                    GameUtils.EKickReason.ManualKick,
                    0,
                    default(DateTime),
                    kickReason
                );
                GameUtils.KickPlayerForClientInfo(cInfo, kickData);

                LogService.Instance.Debug(
                    $"Kicked player {cInfo.playerName} ({args.Player.GameId}): {kickReason}"
                );

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object>
                        {
                            { "success", true },
                            { "playerName", cInfo.playerName },
                            { "reason", kickReason },
                        },
                        requestId
                    )
                );
            });
        }

        public static async Task BanPlayer(string requestId, TakaroBanPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendError(requestId, "Invalid or missing gameId parameter");
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);

                PlatformUserIdentifierAbs userId = PlatformUserIdentifierAbs.FromCombinedString(
                    $"EOS_{args.Player.GameId}"
                );
                PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
                PersistentPlayerData playerData = playerList.GetPlayerData(userId);

                if (playerData == null)
                {
                    SendError(requestId, "Player not found in persistent data");
                    return;
                }

                string playerName = playerData.PlayerName.playerName.Text;
                string banReason = string.IsNullOrEmpty(args.Reason)
                    ? "Banned by admin"
                    : args.Reason;

                DateTime banUntil = DateTime.MaxValue;
                if (!string.IsNullOrEmpty(args.ExpiresAt))
                {
                    try
                    {
                        banUntil = DateTime.Parse(
                            args.ExpiresAt,
                            null,
                            System.Globalization.DateTimeStyles.RoundtripKind
                        );
                        if (banUntil.Kind != DateTimeKind.Utc)
                        {
                            banUntil = banUntil.ToUniversalTime();
                        }
                    }
                    catch (Exception parseEx)
                    {
                        LogService.Instance.Warn(
                            $"Failed to parse ban expiration date '{args.ExpiresAt}': {parseEx.Message}. Using permanent ban."
                        );
                        banUntil = DateTime.MaxValue;
                    }
                }

                bool banSuccess = false;
                string banMethod = "";

                // Timed bans must use AdminTools.Blacklist (BlockedPlayerList has no
                // expiration); permanent bans prefer the platform BlockedPlayerList.
                if (banUntil != DateTime.MaxValue)
                {
                    GameManager.Instance.adminTools.Blacklist.AddBan(
                        "Admin Ban",
                        userId,
                        banUntil,
                        banReason
                    );
                    banSuccess = true;
                    banMethod = "AdminTools.Blacklist (timed)";
                }
                else if (Platform.BlockedPlayerList.Instance != null)
                {
                    var listEntry = Platform.BlockedPlayerList.Instance.AddOrUpdatePlayer(
                        playerData.PlayerData,
                        DateTime.UtcNow,
                        true,
                        false
                    );

                    if (listEntry != null)
                    {
                        banSuccess = true;
                        banMethod = "BlockedPlayerList (permanent)";
                    }
                }

                if (!banSuccess)
                {
                    GameManager.Instance.adminTools.Blacklist.AddBan(
                        "Admin Ban",
                        userId,
                        banUntil,
                        banReason
                    );
                    banSuccess = true;
                    banMethod = "AdminTools.Blacklist";
                }

                // If the player is currently online, kick them immediately
                if (cInfo != null)
                {
                    var kickData = new GameUtils.KickPlayerData(
                        GameUtils.EKickReason.ManualKick,
                        0,
                        default(DateTime),
                        banReason
                    );
                    GameUtils.KickPlayerForClientInfo(cInfo, kickData);
                }

                StateMirror.Instance.RefreshBans();

                LogService.Instance.Debug(
                    $"Banned player {playerName} ({args.Player.GameId}) using {banMethod}: {banReason}, expires: {(banUntil == DateTime.MaxValue ? "never" : banUntil.ToString("o"))}"
                );

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object>
                        {
                            { "success", true },
                            { "playerName", playerName },
                            { "reason", banReason },
                            { "wasOnline", cInfo != null },
                            { "method", banMethod },
                            {
                                "expiresAt",
                                banUntil == DateTime.MaxValue ? null : banUntil.ToString("o")
                            },
                        },
                        requestId
                    )
                );
            });
        }

        public static async Task UnbanPlayer(string requestId, TakaroUnbanPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendError(requestId, "Invalid or missing gameId parameter");
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                PlatformUserIdentifierAbs userId = PlatformUserIdentifierAbs.FromCombinedString(
                    $"EOS_{args.Player.GameId}"
                );
                bool unbanSuccess = false;
                string playerName = "";
                string unbanMethod = "";

                if (Platform.BlockedPlayerList.Instance != null)
                {
                    var blockedEntry = Platform.BlockedPlayerList.Instance.GetPlayerStateInfo(
                        userId
                    );

                    if (blockedEntry != null && blockedEntry.Blocked)
                    {
                        var result = blockedEntry.SetBlockState(false);

                        if (result.Item1)
                        {
                            unbanSuccess = true;
                            playerName = blockedEntry.PlayerData.PlayerName.Text;
                            unbanMethod = "BlockedPlayerList";
                        }
                    }
                }

                if (!unbanSuccess)
                {
                    if (
                        GameManager.Instance.adminTools.Blacklist.IsBanned(
                            userId,
                            out DateTime _,
                            out string _
                        )
                    )
                    {
                        GameManager.Instance.adminTools.Blacklist.RemoveBan(userId);
                        unbanSuccess = true;
                        unbanMethod = "AdminTools.Blacklist";

                        PersistentPlayerList playerList =
                            GameManager.Instance.GetPersistentPlayerList();
                        PersistentPlayerData playerData = playerList.GetPlayerData(userId);
                        playerName =
                            playerData != null
                                ? playerData.PlayerName.playerName.Text
                                : $"Player_{args.Player.GameId}";
                    }
                }

                if (!unbanSuccess)
                {
                    SendError(requestId, "Player not found in ban list or failed to unban");
                    return;
                }

                StateMirror.Instance.RefreshBans();

                LogService.Instance.Debug(
                    $"Unbanned player {playerName} ({args.Player.GameId}) using {unbanMethod}"
                );

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object>
                        {
                            { "success", true },
                            { "playerName", playerName },
                            { "method", unbanMethod },
                        },
                        requestId
                    )
                );
            });
        }

        public static async Task TeleportPlayer(string requestId, TakaroTeleportPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendError(requestId, "Invalid or missing gameId parameter");
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
                if (cInfo == null)
                {
                    SendError(requestId, "Player not found");
                    return;
                }

                if (
                    !GameManager.Instance.World.Players.dict.TryGetValue(
                        cInfo.entityId,
                        out EntityPlayer player
                    )
                )
                {
                    SendError(requestId, "Player entity not found");
                    return;
                }

                if (!player.IsSpawned())
                {
                    SendError(requestId, "Player is not spawned");
                    return;
                }

                if (player.IsDead())
                {
                    SendError(requestId, "Player is dead");
                    return;
                }

                Vector3 targetPosition = new Vector3(args.X, args.Y, args.Z);
                ClampToWorldBounds(ref targetPosition);

                cInfo.SendPackage(
                    NetPackageManager
                        .GetPackage<NetPackageTeleportPlayer>()
                        .Setup(targetPosition, null, false)
                );

                LogService.Instance.Debug(
                    $"Teleported player {cInfo.playerName} to ({targetPosition.x}, {targetPosition.y}, {targetPosition.z})"
                );

                Send(
                    WebSocketMessage.Create(
                        WebSocketMessage.MessageTypes.Response,
                        new Dictionary<string, object>
                        {
                            { "success", true },
                            { "playerName", cInfo.playerName },
                            { "x", targetPosition.x },
                            { "y", targetPosition.y },
                            { "z", targetPosition.z },
                        },
                        requestId
                    )
                );
            });
        }

        public static async Task Shutdown(string requestId)
        {
            LogService.Instance.Info("Initiating immediate server shutdown");

            var tcs = new TaskCompletionSource<string>();
            var cr = new CommandResult("shutdown", tcs);
            SdtdConsole.Instance.ExecuteAsync("shutdown", cr);
            await tcs.Task;

            Send(WebSocketMessage.CreateResponse(requestId, null));
        }

        private static void ClampToWorldBounds(ref Vector3 position)
        {
            float x = position.x;
            float z = position.z;
            float positiveX,
                positiveY,
                negativeX,
                negativeY;

            string gameWorld = GamePrefs.GetString(EnumGamePrefs.GameWorld);
            if (gameWorld.ToLower() == "navezgane")
            {
                positiveX = 3000;
                positiveY = 3000;
                negativeX = -3000;
                negativeY = -3000;
            }
            else
            {
                IChunkProvider chunkProvider = GameManager.Instance.World.ChunkCache.ChunkProvider;
                positiveX = chunkProvider.GetWorldSize().x;
                positiveY = chunkProvider.GetWorldSize().y;
                negativeX = chunkProvider.GetWorldSize().x * -1;
                negativeY = chunkProvider.GetWorldSize().y * -1;
            }

            bool outside = false;
            if (x >= positiveX)
            {
                outside = true;
                x = positiveX - 10;
            }
            else if (x <= negativeX)
            {
                outside = true;
                x = negativeX + 10;
            }

            if (z >= positiveY)
            {
                outside = true;
                z = positiveY - 10;
            }
            else if (z <= negativeY)
            {
                outside = true;
                z = negativeY + 10;
            }

            if (outside)
            {
                LogService.Instance.Warn(
                    $"Teleport coordinates ({position.x}, {position.z}) were outside world bounds, adjusted to ({x}, {z})"
                );
                position = new Vector3(x, position.y, z);
            }
        }

        private static void Send(WebSocketMessage message)
        {
            WebSocketTransport.Instance.Send(message);
        }

        private static void SendError(string requestId, string errorMessage)
        {
            WebSocketTransport.Instance.SendErrorResponse(requestId, errorMessage);
        }
    }
}
