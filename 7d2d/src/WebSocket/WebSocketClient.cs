using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using Newtonsoft.Json;
using Takaro.Config;
using WebSocketSharp;
using UnityEngine;
using System.Threading.Tasks;
using Takaro.Services;

namespace Takaro.WebSocket
{
    public class WebSocketArgs<T>
    {
        public static T Parse(object argsObject)
        {
            try
            {
                if (argsObject == null)
                    return default;

                // Handle case where args is already a string that needs deserialization
                if (argsObject is string argsString)
                {
                    return JsonConvert.DeserializeObject<T>(argsString);
                }

                // Handle case where args is already a JObject or Dictionary
                if (argsObject is Newtonsoft.Json.Linq.JObject jObject)
                {
                    return jObject.ToObject<T>();
                }

                if (argsObject is Dictionary<string, object> dict)
                {
                    string json = JsonConvert.SerializeObject(dict);
                    return JsonConvert.DeserializeObject<T>(json);
                }

                // As a last resort, try direct conversion
                return (T)Convert.ChangeType(argsObject, typeof(T));
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error parsing WebSocket args: {ex.Message}");
                return default;
            }
        }
    }

    public class TakaroPlayerReferenceArgs
    {
        public string GameId { get; set; }
    }

    public class TakaroGiveItemArgs
    {
        public string GameId { get; set; }
        public string Item { get; set; }
        public int Amount { get; set; }
        public string Quality { get; set; }
    }

    public class TakaroExecuteCommandArgs 
    {
        public string Command { get; set; }
    }

    public class TakaroSendMessageArgs 
    {
        public string Message { get; set; }
        public TakaroSendMessageRecipientArgs Recipient { get; set; }
    }

    public class TakaroSendMessageRecipientArgs
    {
        public string GameId { get; set; }
    }

    public class TakaroPlayerReference
    {
        public string GameId { get; set; }
    }

    public class TakaroKickPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public string Reason { get; set; }
    }

    public class TakaroBanPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public string Reason { get; set; }
        public string ExpiresAt { get; set; }
    }

    public class TakaroUnbanPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
    }

    public class TakaroTeleportPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
    }


    public class WebSocketClient
    {
        private static WebSocketClient _instance;
        private static readonly object _lock = new object();

        private WebSocketSharp.WebSocket _webSocket;
        private Timer _heartbeatTimer;
        private Timer _reconnectTimer;
        private bool _isConnected = false;
        private bool _shuttingDown = false;
        private int _reconnectAttempts = 0;
        private const int MAX_RECONNECT_ATTEMPTS = 5;

        public static WebSocketClient Instance
        {
            get
            {
                if (_instance == null)
                {
                    lock (_lock)
                    {
                        if (_instance == null)
                        {
                            _instance = new WebSocketClient();
                        }
                    }
                }
                return _instance;
            }
        }

        private WebSocketClient()
        {
            // Private constructor for singleton
        }

        public void Initialize()
        {
            try
            {
                var config = ConfigManager.Instance;
                if (!config.WebSocketEnabled)
                {
                    LogService.Instance.Info(
                        "WebSocket client is disabled in config. Skipping initialization."
                    );
                    return;
                }

                LogService.Instance.Info($"Initializing WebSocket client to {config.WebSocketUrl}");

                ConnectToServer();
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error initializing WebSocket client: {ex.Message}");
                Log.Exception(ex);
            }
        }

        public void Shutdown()
        {
            _shuttingDown = true;
            StopTimers();
            CloseConnection();
        }

        private void ConnectToServer()
        {
            try
            {
                var config = ConfigManager.Instance;
                if (string.IsNullOrEmpty(config.WebSocketUrl))
                {
                    LogService.Instance.Error("WebSocket URL is not set in config.");
                    return;
                }

                _webSocket = new WebSocketSharp.WebSocket(config.WebSocketUrl);

                _webSocket.OnOpen += (sender, e) =>
                {
                    _isConnected = true;
                    _reconnectAttempts = 0;
                    LogService.Instance.Info("WebSocket connection established");

                    // Send registration message
                    if (
                        string.IsNullOrEmpty(config.RegistrationToken)
                        || string.IsNullOrEmpty(config.IdentityToken)
                    )
                    {
                        LogService.Instance.Error(
                            "Registration token or identity token is not set in config."
                        );
                        return;
                    }

                    SendMessage(
                        WebSocketMessage.CreateIdentify(
                            config.RegistrationToken,
                            config.IdentityToken
                        )
                    );
                    // Start heartbeat
                    StartHeartbeat();
                };

                _webSocket.OnMessage += (sender, e) =>
                {
                    HandleMessage(e.Data);
                };

                _webSocket.OnError += (sender, e) =>
                {
                    LogService.Instance.Error($"WebSocket error: {e.Message}");
                };

                _webSocket.OnClose += (sender, e) =>
                {
                    _isConnected = false;
                    LogService.Instance.Info($"WebSocket connection closed: {e.Code} - {e.Reason}");

                    StopTimers();

                    if (!_shuttingDown)
                    {
                        ScheduleReconnect();
                    }
                };

                _webSocket.Connect();
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error connecting to WebSocket server: {ex.Message}");
                Log.Exception(ex);
                ScheduleReconnect();
            }
        }

        #region HandleMessage
        private void HandleMessage(string message)
        {
            try
            {
                LogService.Instance.Debug($"Received WebSocket message: {message}");
                var webSocketMessage = JsonConvert.DeserializeObject<WebSocketMessage>(message);

                if (webSocketMessage == null || webSocketMessage.Payload == null)
                {
                    // No data in the message, so nothing to do
                    return;
                }

                string requestId = webSocketMessage.RequestId;

                if (string.IsNullOrEmpty(requestId))
                {
                    LogService.Instance.Warn("Received message without requestId");
                    return;
                }

                // Handle the Payload property which could be a dictionary or array
                Dictionary<string, object> payloadDict =
                    webSocketMessage.Payload as Dictionary<string, object>;

                if (payloadDict == null)
                {
                    if (webSocketMessage.Payload is Newtonsoft.Json.Linq.JObject jObject)
                    {
                        payloadDict = jObject.ToObject<Dictionary<string, object>>();
                    }
                    else
                    {
                        LogService.Instance.Warn(
                            "Received message with payload that is not a dictionary"
                        );
                        return;
                    }
                }

                string action = null;
                if (payloadDict.ContainsKey("action"))
                {
                    action = payloadDict["action"].ToString();
                }
                else
                {
                    // No action in the message, so nothing to do
                    return;
                }

                // Extract args if present
                object args = null;
                if (payloadDict.ContainsKey("args"))
                {
                    args = payloadDict["args"];
                }

                try
                {
                                    // Handle different message types
                switch (action)
                {
                    case "testReachability":
                        HandleTestReachability(requestId);
                        break;
                    case "getPlayers":
                        HandleGetPlayers(requestId);
                        break;
                    case "getPlayer":
                        var playerArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (playerArgs == null || string.IsNullOrEmpty(playerArgs.GameId))
                        {
                            SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        HandleGetPlayer(requestId, playerArgs.GameId);
                        break;
                    case "getPlayerLocation":
                        var locationArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (locationArgs == null || string.IsNullOrEmpty(locationArgs.GameId))
                        {
                            SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        HandleGetPlayerLocation(requestId, locationArgs.GameId);
                        break;
                    case "getPlayerInventory":
                        var inventoryArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (inventoryArgs == null || string.IsNullOrEmpty(inventoryArgs.GameId))
                        {
                            SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        HandleGetPlayerInventory(requestId, inventoryArgs.GameId);
                        break;
                    case "listItems":
                        HandleListItems(requestId);
                        break;
                    case "listBans":
                        HandleListBans(requestId);
                        break;
                    case "giveItem":
                        var giveItemArgs = WebSocketArgs<TakaroGiveItemArgs>.Parse(args);
                        HandleGiveItem(requestId, giveItemArgs);
                        break;
                    case "executeConsoleCommand":
                        var executeCommandArgs = WebSocketArgs<TakaroExecuteCommandArgs>.Parse(args);
                        _ = HandleExecuteCommand(requestId, executeCommandArgs);
                        break;
                    case "sendMessage":
                        var sendMessageArgs = WebSocketArgs<TakaroSendMessageArgs>.Parse(args);
                        HandleSendMessage(requestId, sendMessageArgs);
                        break;
                    case "kickPlayer":
                        var kickPlayerArgs = WebSocketArgs<TakaroKickPlayerArgs>.Parse(args);
                        HandleKickPlayer(requestId, kickPlayerArgs);
                        break;
                    case "banPlayer":
                        var banPlayerArgs = WebSocketArgs<TakaroBanPlayerArgs>.Parse(args);
                        HandleBanPlayer(requestId, banPlayerArgs);
                        break;
                    case "unbanPlayer":
                        var unbanPlayerArgs = WebSocketArgs<TakaroUnbanPlayerArgs>.Parse(args);
                        HandleUnbanPlayer(requestId, unbanPlayerArgs);
                        break;
                    case "teleportPlayer":
                        var teleportPlayerArgs = WebSocketArgs<TakaroTeleportPlayerArgs>.Parse(args);
                        HandleTeleportPlayer(requestId, teleportPlayerArgs);
                        break;
                    case "shutdown":
                        _ = HandleShutdown(requestId);
                        break;
                    default:
                        LogService.Instance.Warn($"Unknown message type: {action}");
                        SendErrorResponse(requestId, $"Unknown message type: {action}");
                        break;
                }
                }
                catch (System.Exception)
                {
                    SendErrorResponse(requestId, "Error processing request");
                    throw;
                }


            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error handling WebSocket message: {ex.Message}");
                Log.Exception(ex);
            }
        }
        #endregion

        #region Helpers
        public void SendMessage(WebSocketMessage message)
        {
            try
            {
                if (_webSocket == null || !_isConnected)
                {
                    LogService.Instance.Warn("Cannot send message - WebSocket not connected");
                    return;
                }

                string json = SerializeToJson(message);
                LogService.Instance.Debug($"Sending WebSocket message: {json}");
                _webSocket.Send(json);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error sending WebSocket message: {ex.Message}");
                Log.Exception(ex);
            }
        }

        private void SendErrorResponse(string requestId, string errorMessage)
        {
            WebSocketMessage message = WebSocketMessage.CreateErrorResponse(
                requestId,
                errorMessage
            );
            SendMessage(message);
        }

        private string SerializeToJson(WebSocketMessage message)
        {
            return Newtonsoft.Json.JsonConvert.SerializeObject(message);
        }

        private void StartHeartbeat()
        {
            StopHeartbeatTimer();

            _heartbeatTimer = new Timer(
                state =>
                {
                    if (_isConnected)
                    {
                        SendMessage(WebSocketMessage.CreateHeartbeat());
                    }
                },
                null,
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(30)
            );
        }

        private void StopHeartbeatTimer()
        {
            if (_heartbeatTimer != null)
            {
                _heartbeatTimer.Dispose();
                _heartbeatTimer = null;
            }
        }

        private void ScheduleReconnect()
        {
            if (_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS)
            {
                LogService.Instance.Error(
                    $"Maximum reconnection attempts ({MAX_RECONNECT_ATTEMPTS}) reached. Giving up."
                );
                return;
            }

            _reconnectAttempts++;
            var interval = TimeSpan.FromSeconds(ConfigManager.Instance.ReconnectIntervalSeconds);
            LogService.Instance.Info(
                $"Scheduling reconnect attempt {_reconnectAttempts} in {interval.TotalSeconds} seconds"
            );

            _reconnectTimer = new Timer(
                state =>
                {
                    ConnectToServer();
                },
                null,
                interval,
                Timeout.InfiniteTimeSpan
            );
        }

        private void StopTimers()
        {
            StopHeartbeatTimer();

            if (_reconnectTimer != null)
            {
                _reconnectTimer.Dispose();
                _reconnectTimer = null;
            }
        }

        private void CloseConnection()
        {
            if (_webSocket != null && _isConnected)
            {
                try
                {
                    _webSocket.Close(CloseStatusCode.Normal, "Application shutting down");
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error($"Error closing WebSocket connection: {ex.Message}");
                }
                finally
                {
                    _webSocket = null;
                    _isConnected = false;
                }
            }
        }

        #endregion
        #region Action Handlers

        private void HandleTestReachability(string requestId)
        {
            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                new Dictionary<string, object> { { "connectable", true } },
                requestId
            );
            SendMessage(message);
        }

        private void HandleGetPlayers(string requestId)
        {
            List<TakaroPlayer> players = new List<TakaroPlayer>();
            foreach (var player in GameManager.Instance.World.Players.list)
            {
                int entityId = player.entityId;
                ClientInfo cInfo = ConnectionManager.Instance.Clients.ForEntityId(entityId);

                TakaroPlayer takaroPlayer = Shared.TransformClientInfoToTakaroPlayer(cInfo);
                if (takaroPlayer != null)
                {
                    players.Add(takaroPlayer);
                }
            }

            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                players.ToArray(),
                requestId
            );
            SendMessage(message);
        }

        private void HandleGetPlayer(string requestId, string gameId)
        {
            ClientInfo cInfo = Shared.GetClientInfoFromGameId(gameId);
            if (cInfo == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }

            TakaroPlayer takaroPlayer = Shared.TransformClientInfoToTakaroPlayer(cInfo);
            if (takaroPlayer == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }

            WebSocketMessage message = WebSocketMessage.CreateResponse(requestId, takaroPlayer);
            SendMessage(message);
        }

        private void HandleGetPlayerLocation(string requestId, string gameId)
        {
            ClientInfo cInfo = Shared.GetClientInfoFromGameId(gameId);
            if (cInfo == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }

            EntityPlayer player = GameManager.Instance.World.Players.dict[cInfo.entityId];
            if (player == null)
            {
                SendErrorResponse(requestId, "Player entity not found");
                return;
            }

            Vector3i pos = new Vector3i(player.GetPosition());
            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                new Dictionary<string, object>
                {
                    { "x", pos.x },
                    { "y", pos.y },
                    { "z", pos.z }
                },
                requestId
            );
            SendMessage(message);
        }

        private void HandleGetPlayerInventory(string requestId, string gameId)
        {
            ClientInfo cInfo = Shared.GetClientInfoFromGameId(gameId);
            if (cInfo == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }

            List<TakaroItem> items = new List<TakaroItem>();

            ProcessItemStacks(cInfo.latestPlayerData.inventory, items);
            ProcessItemStacks(cInfo.latestPlayerData.bag, items);
            ProcessEquippedItems(cInfo.latestPlayerData.equipment.GetItems(), items);

            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                items.ToArray(),
                requestId
            );
            SendMessage(message);
        }

        private void ProcessItemStacks(ItemStack[] itemStacks, List<TakaroItem> itemsList)
        {
            if (itemStacks == null)
                return;

            foreach (var item in itemStacks)
            {
                ItemValue itemValue = item.itemValue;

                if (itemValue == null || itemValue.Equals(ItemValue.None))
                {
                    continue;
                }

                ItemClass itemClass = itemValue.ItemClass;
                TakaroItem takaroItem = Shared.TransformItemToTakaroItem(itemClass);
                takaroItem.Amount = item.count;
                takaroItem.Quality = itemValue.Quality.ToString();
                itemsList.Add(takaroItem);
            }
        }

        private void ProcessEquippedItems(ItemValue[] equippedItems, List<TakaroItem> itemsList)
        {
            if (equippedItems == null)
                return;

            foreach (var itemValue in equippedItems)
            {
                if (itemValue == null || itemValue.Equals(ItemValue.None))
                {
                    continue;
                }

                ItemClass itemClass = itemValue.ItemClass;
                TakaroItem takaroItem = Shared.TransformItemToTakaroItem(itemClass);
                takaroItem.Amount = 1;
                takaroItem.Quality = itemValue.Quality.ToString();
                itemsList.Add(takaroItem);
            }
        }

        private void HandleListItems(string requestId)
        {
            List<TakaroItem> allItems = new List<TakaroItem>();
            for (int i = 0; i < ItemClass.itemNames.Count; i++) {
				string itemName = ItemClass.itemNames [i];
                ItemClass item = ItemClass.nameToItem[itemName];
                if (item == null) {
                    continue;
                }
                allItems.Add(Shared.TransformItemToTakaroItem(item));

			}
            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                allItems.ToArray(),
                requestId
            );
            SendMessage(message);
        }

        private void HandleListBans(string requestId)
        {
            List<TakaroBan> bans = new List<TakaroBan>();
            
            try
            {
                // Try modern BlockedPlayerList first, fallback to AdminTools
                if (Platform.BlockedPlayerList.Instance != null)
                {
                    // Get all blocked players using the proper BlockedPlayerList API
                    foreach (var blockedEntry in Platform.BlockedPlayerList.Instance.GetEntriesOrdered(true, false))
                    {
                        if (blockedEntry?.PlayerData == null) continue;

                        TakaroPlayer takaroPlayer = new TakaroPlayer
                        {
                            // Use the CrossPlatform ID as the primary GameId (without EOS_ prefix)
                            GameId = blockedEntry.PlayerData.PrimaryId.CombinedString.Replace("EOS_", ""),
                            Name = blockedEntry.PlayerData.PlayerName.Text
                        };

                        // Set platform-specific IDs based on the platform type
                        string primaryId = blockedEntry.PlayerData.PrimaryId.CombinedString;
                        if (primaryId.StartsWith("EOS_"))
                        {
                            takaroPlayer.EpicOnlineServicesId = primaryId.Replace("EOS_", "");
                        }

                        // Check if there's a different native platform ID
                        if (blockedEntry.PlayerData.NativeId != null && 
                            blockedEntry.PlayerData.NativeId.CombinedString != primaryId)
                        {
                            string nativeId = blockedEntry.PlayerData.NativeId.CombinedString;
                            if (nativeId.StartsWith("Steam_"))
                            {
                                takaroPlayer.SteamId = nativeId.Replace("Steam_", "");
                            }
                            else if (nativeId.StartsWith("XBL_"))
                            {
                                takaroPlayer.XboxLiveId = nativeId.Replace("XBL_", "");
                            }
                        }

                        TakaroBan takaroBan = new TakaroBan
                        {
                            Player = takaroPlayer,
                            Reason = "Blocked", // BlockedPlayerList doesn't store reasons, use default
                            ExpiresAt = null // BlockedPlayerList doesn't store expiration, permanent bans
                        };
                        bans.Add(takaroBan);
                    }
                }
                else
                {
                    // Fallback to AdminTools blacklist system
                    PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
                    foreach (var ban in GameManager.Instance.adminTools.Blacklist.GetBanned())
                    {
                        // Support all platform types, not just EOS
                        PersistentPlayerData playerData = playerList.GetPlayerData(ban.UserIdentifier);
                        if (playerData == null) continue;

                        TakaroPlayer takaroPlayer = new TakaroPlayer
                        {
                            GameId = ban.UserIdentifier.CombinedString.Replace("EOS_", ""),
                            Name = playerData.PlayerName.playerName.Text
                        };

                        // Set platform-specific IDs
                        string platformId = ban.UserIdentifier.CombinedString;
                        if (platformId.StartsWith("EOS_"))
                        {
                            takaroPlayer.EpicOnlineServicesId = platformId.Replace("EOS_", "");
                        }
                        else if (platformId.StartsWith("Steam_"))
                        {
                            takaroPlayer.SteamId = platformId.Replace("Steam_", "");
                        }
                        else if (platformId.StartsWith("XBL_"))
                        {
                            takaroPlayer.XboxLiveId = platformId.Replace("XBL_", "");
                        }

                        TakaroBan takaroBan = new TakaroBan
                        {
                            Player = takaroPlayer,
                            Reason = ban.BanReason,
                            ExpiresAt = ban.BannedUntil.ToString("o")
                        };
                        bans.Add(takaroBan);
                    }
                }

                WebSocketMessage message = WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    bans.ToArray(),
                    requestId
                );
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error listing bans: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to list bans: {ex.Message}");
            }
        }

        private void HandleGiveItem(string requestId, TakaroGiveItemArgs args)
        {
            if (args == null || args.GameId == null || string.IsNullOrEmpty(args.Item))
            {
                SendErrorResponse(requestId, "Invalid or missing parameters");
                return;
            }
            
            ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.GameId);
            if (cInfo == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }
            
            ItemValue itemValue = ItemClass.GetItem(args.Item);
            if (itemValue == null || itemValue.type == ItemValue.None.type)
            {
                SendErrorResponse(requestId, "Item not found");
                return;
            }
            
            if(!GameManager.Instance.World.Players.dict.TryGetValue(cInfo.entityId, out EntityPlayer player))
            {
                SendErrorResponse(requestId, "Player entity not found");
                return;
            }
            
            if(!player.IsSpawned())
            {
                SendErrorResponse(requestId, "Player is not spawned");
                return;
            }
            
            if(player.IsDead())
            {
                SendErrorResponse(requestId, "Player is dead");
                return;
            }
            
            if (args.Amount <= 0)
            {
                SendErrorResponse(requestId, "Invalid item amount");
                return;
            }

            // Parse quality parameter or use default max quality
            ushort quality = Constants.cItemMaxQuality;
            if (!string.IsNullOrEmpty(args.Quality))
            {
                if (ushort.TryParse(args.Quality, out ushort parsedQuality) && 
                    parsedQuality >= 0 && 
                    parsedQuality <= Constants.cItemMaxQuality)
                {
                    quality = parsedQuality;
                }
                else
                {
                    SendErrorResponse(requestId, "Invalid quality value");
                    return;
                }
            }
            
            // Create a new ItemValue with appropriate quality
            ItemValue iv = new ItemValue(itemValue.type, true);
            
            // Handle quality for items with sub-items or that have quality
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
            
            // Create the item stack with the specified amount
            ItemStack itemStack = new ItemStack(iv, args.Amount);
            World world = GameManager.Instance.World;
            EntityItem entityItem = (EntityItem)EntityFactory.CreateEntity(new EntityCreationData
            {
                entityClass = EntityClass.FromString("item"),
                id = EntityFactory.nextEntityID++,
                itemStack = itemStack,
                pos = world.Players.dict[cInfo.entityId].position,
                rot = new Vector3(20f, 0f, 20f),
                lifetime = 60f,
                belongsPlayerId = cInfo.entityId
            });
            world.SpawnEntityInWorld(entityItem);
            cInfo.SendPackage(NetPackageManager.GetPackage<NetPackageEntityCollect>().Setup(entityItem.entityId, cInfo.entityId));
            world.RemoveEntity(entityItem.entityId, EnumRemoveEntityReason.Despawned);
            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                new Dictionary<string, object> {},
                requestId
            );
            SendMessage(message);
        }

        private void HandleSendMessage(string requestId, TakaroSendMessageArgs args)
        {
            if (args == null || string.IsNullOrEmpty(args.Message))
            {
                SendErrorResponse(requestId, "Invalid or missing parameters");
                return;
            }

            // If a GameId is provided, send the message to that player as a whisper
            if(args.Recipient != null && args.Recipient.GameId != null) {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Recipient.GameId);
                if (cInfo == null)
                {
                    SendErrorResponse(requestId, "Player not found");
                    return;
                }

                cInfo.SendPackage (NetPackageManager.GetPackage<NetPackageChat> ().Setup (EChatType.Whisper, -1,args.Message, null, EMessageSender.Server, GeneratedTextManager.BbCodeSupportMode.Supported));
            // Otherwise, send a global message
            } else {
                GameManager.Instance.ChatMessageServer(null, EChatType.Global, -1, args.Message, null, EMessageSender.Server);
            }

            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                new Dictionary<string, object> {},
                requestId
            );
            SendMessage(message);
        }

        private async Task HandleExecuteCommand(string requestId, TakaroExecuteCommandArgs args)
        {
            if (args == null || string.IsNullOrEmpty(args.Command))
            {
                SendErrorResponse(requestId, "Invalid or missing command");
                return;
            }
            var tcs = new TaskCompletionSource<string>();
            var cr = new CommandResult(args.Command, tcs);
            SdtdConsole.Instance.ExecuteAsync (args.Command, cr);
            string result = await tcs.Task;

            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.Response,
                new Dictionary<string, object> { 
                    { "rawResult", result },
                    {"success", true}
                     },
                requestId
            );
            SendMessage(message);
        }

        private void HandleKickPlayer(string requestId, TakaroKickPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                return;
            }

            ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
            if (cInfo == null)
            {
                SendErrorResponse(requestId, "Player not found");
                return;
            }

            // Prepare kick reason - default to "Kicked by admin" if no reason provided
            string kickReason = string.IsNullOrEmpty(args.Reason) ? "Kicked by admin" : args.Reason;

            try
            {
                // Use GameUtils.KickPlayerForClientInfo as discovered from decompiled code
                // This follows the same pattern as the console kick command
                var kickData = new GameUtils.KickPlayerData(
                    GameUtils.EKickReason.ManualKick,
                    0,
                    default(DateTime),
                    kickReason
                );

                GameUtils.KickPlayerForClientInfo(cInfo, kickData);

                LogService.Instance.Debug($"Kicked player {cInfo.playerName} ({args.Player.GameId}): {kickReason}");

                WebSocketMessage message = WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> 
                    {
                        { "success", true },
                        { "playerName", cInfo.playerName },
                        { "reason", kickReason }
                    },
                    requestId
                );
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error kicking player {args.Player.GameId}: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to kick player: {ex.Message}");
            }
        }

        private void HandleBanPlayer(string requestId, TakaroBanPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                return;
            }

            try
            {
                // First, try to find if the player is currently online
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
                
                // Get persistent player data - this works for both online and offline players
                PlatformUserIdentifierAbs userId = PlatformUserIdentifierAbs.FromCombinedString($"EOS_{args.Player.GameId}");
                PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
                PersistentPlayerData playerData = playerList.GetPlayerData(userId);
                
                if (playerData == null)
                {
                    SendErrorResponse(requestId, "Player not found in persistent data");
                    return;
                }

                string playerName = playerData.PlayerName.playerName.Text;
                string banReason = string.IsNullOrEmpty(args.Reason) ? "Banned by admin" : args.Reason;
                
                // Parse the expiration date from args
                DateTime banUntil = DateTime.MaxValue; // Default to permanent ban
                if (!string.IsNullOrEmpty(args.ExpiresAt))
                {
                    try
                    {
                        // Parse ISO 8601 date format
                        banUntil = DateTime.Parse(args.ExpiresAt, null, System.Globalization.DateTimeStyles.RoundtripKind);
                        // Ensure the date is in UTC
                        if (banUntil.Kind != DateTimeKind.Utc)
                        {
                            banUntil = banUntil.ToUniversalTime();
                        }
                        LogService.Instance.Debug($"Parsed ban expiration date: {banUntil:o}");
                    }
                    catch (Exception parseEx)
                    {
                        LogService.Instance.Warn($"Failed to parse ban expiration date '{args.ExpiresAt}': {parseEx.Message}. Using permanent ban.");
                        banUntil = DateTime.MaxValue;
                    }
                }
                
                bool banSuccess = false;
                string banMethod = "";

                // Use AdminTools.Blacklist for timed bans, BlockedPlayerList for permanent bans
                if (banUntil != DateTime.MaxValue)
                {
                    // Timed ban - must use AdminTools.Blacklist (BlockedPlayerList doesn't support expiration)
                    try
                    {
                        GameManager.Instance.adminTools.Blacklist.AddBan("Admin Ban", userId, banUntil, banReason);
                        banSuccess = true;
                        banMethod = "AdminTools.Blacklist (timed)";
                    }
                    catch (Exception adminEx)
                    {
                        LogService.Instance.Warn($"AdminTools blacklist failed for timed ban: {adminEx.Message}");
                    }
                }
                else if (Platform.BlockedPlayerList.Instance != null)
                {
                    // Permanent ban - try modern BlockedPlayerList first
                    var listEntry = Platform.BlockedPlayerList.Instance.AddOrUpdatePlayer(
                        playerData.PlayerData, 
                        DateTime.UtcNow, 
                        true, // blocked = true
                        false // ignoreLimit = false
                    );

                    if (listEntry != null)
                    {
                        banSuccess = true;
                        banMethod = "BlockedPlayerList (permanent)";
                    }
                }
                
                if (!banSuccess)
                {
                    // Final fallback to AdminTools blacklist system
                    try
                    {
                        GameManager.Instance.adminTools.Blacklist.AddBan("Admin Ban", userId, banUntil, banReason);
                        banSuccess = true;
                        banMethod = "AdminTools.Blacklist";
                    }
                    catch (Exception adminEx)
                    {
                        LogService.Instance.Warn($"AdminTools blacklist failed: {adminEx.Message}");
                    }
                }

                if (!banSuccess)
                {
                    SendErrorResponse(requestId, "Failed to ban player - both BlockedPlayerList and AdminTools unavailable");
                    return;
                }

                // If player is currently online, kick them immediately
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

                LogService.Instance.Debug($"Banned player {playerName} ({args.Player.GameId}) using {banMethod}: {banReason}, expires: {(banUntil == DateTime.MaxValue ? "never" : banUntil.ToString("o"))}");

                WebSocketMessage message = WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> 
                    {
                        { "success", true },
                        { "playerName", playerName },
                        { "reason", banReason },
                        { "wasOnline", cInfo != null },
                        { "method", banMethod },
                        { "expiresAt", banUntil == DateTime.MaxValue ? null : banUntil.ToString("o") }
                    },
                    requestId
                );
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error banning player {args.Player.GameId}: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to ban player: {ex.Message}");
            }
        }

        private void HandleUnbanPlayer(string requestId, TakaroUnbanPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                return;
            }

            try
            {
                PlatformUserIdentifierAbs userId = PlatformUserIdentifierAbs.FromCombinedString($"EOS_{args.Player.GameId}");
                bool unbanSuccess = false;
                string playerName = "";
                string unbanMethod = "";

                // Try modern BlockedPlayerList first, fallback to AdminTools
                if (Platform.BlockedPlayerList.Instance != null)
                {
                    var blockedEntry = Platform.BlockedPlayerList.Instance.GetPlayerStateInfo(userId);
                    
                    if (blockedEntry != null && blockedEntry.Blocked)
                    {
                        // Use the SetBlockState method to unban the player
                        var result = blockedEntry.SetBlockState(false);
                        
                        if (result.Item1) // result.Item1 is success bool
                        {
                            unbanSuccess = true;
                            playerName = blockedEntry.PlayerData.PlayerName.Text;
                            unbanMethod = "BlockedPlayerList";
                        }
                    }
                }
                
                if (!unbanSuccess)
                {
                    // Fallback to AdminTools blacklist system
                    try
                    {
                        // Check if player is banned in AdminTools blacklist
                        DateTime bannedUntil;
                        string banReason;
                        if (GameManager.Instance.adminTools.Blacklist.IsBanned(userId, out bannedUntil, out banReason))
                        {
                            GameManager.Instance.adminTools.Blacklist.RemoveBan(userId);
                            unbanSuccess = true;
                            unbanMethod = "AdminTools.Blacklist";
                            
                            // Get player name from persistent data
                            PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
                            PersistentPlayerData playerData = playerList.GetPlayerData(userId);
                            if (playerData != null)
                            {
                                playerName = playerData.PlayerName.playerName.Text;
                            }
                            else
                            {
                                playerName = $"Player_{args.Player.GameId}";
                            }
                        }
                    }
                    catch (Exception adminEx)
                    {
                        LogService.Instance.Warn($"AdminTools unban failed: {adminEx.Message}");
                    }
                }

                if (!unbanSuccess)
                {
                    SendErrorResponse(requestId, "Player not found in ban list or failed to unban");
                    return;
                }

                LogService.Instance.Debug($"Unbanned player {playerName} ({args.Player.GameId}) using {unbanMethod}");

                WebSocketMessage message = WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> 
                    {
                        { "success", true },
                        { "playerName", playerName },
                        { "method", unbanMethod }
                    },
                    requestId
                );
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error unbanning player {args.Player.GameId}: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to unban player: {ex.Message}");
            }
        }

        private void HandleTeleportPlayer(string requestId, TakaroTeleportPlayerArgs args)
        {
            if (args == null || args.Player == null || string.IsNullOrEmpty(args.Player.GameId))
            {
                SendErrorResponse(requestId, "Invalid or missing gameId parameter");
                return;
            }

            try
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
                if (cInfo == null)
                {
                    SendErrorResponse(requestId, "Player not found");
                    return;
                }

                // Get the player entity
                if (!GameManager.Instance.World.Players.dict.TryGetValue(cInfo.entityId, out EntityPlayer player))
                {
                    SendErrorResponse(requestId, "Player entity not found");
                    return;
                }

                // Validate player state
                if (!player.IsSpawned())
                {
                    SendErrorResponse(requestId, "Player is not spawned");
                    return;
                }

                if (player.IsDead())
                {
                    SendErrorResponse(requestId, "Player is dead");
                    return;
                }

                // Validate coordinates
                Vector3 targetPosition = new Vector3(args.X, args.Y, args.Z);

                // Check world bounds using the same logic as ServerTools
                CheckWorldBounds(cInfo, ref targetPosition);

                // Log the teleportation attempt
                LogService.Instance.Debug($"Teleporting player {cInfo.playerName} ({args.Player.GameId}) to ({args.X}, {args.Y}, {args.Z})");

                // Perform the teleportation using the network packet system (like ServerTools)
                cInfo.SendPackage(NetPackageManager.GetPackage<NetPackageTeleportPlayer>().Setup(
                    targetPosition, null, false));

                LogService.Instance.Debug($"Successfully teleported player {cInfo.playerName} to ({targetPosition.x}, {targetPosition.y}, {targetPosition.z})");

                WebSocketMessage message = WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> 
                    {
                        { "success", true },
                        { "playerName", cInfo.playerName },
                        { "x", targetPosition.x },
                        { "y", targetPosition.y },
                        { "z", targetPosition.z }
                    },
                    requestId
                );
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error teleporting player {args.Player.GameId}: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to teleport player: {ex.Message}");
            }
        }

        private void CheckWorldBounds(ClientInfo cInfo, ref Vector3 position)
        {
            float x = position.x;
            float z = position.z;
            float positiveX, positiveY, negativeX, negativeY;

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
                LogService.Instance.Warn($"Teleport coordinates ({position.x}, {position.z}) were outside world bounds, adjusted to ({x}, {z})");
                position = new Vector3(x, position.y, z);
            }
        }

        private async Task HandleShutdown(string requestId)
        {
            try
            {
                // Use vanilla 7D2D shutdown command (no delay parameter in vanilla)
                string reason = "Server shutdown requested";

                // Use the proper vanilla 7D2D shutdown command
                string shutdownCommand = "shutdown";
                
                LogService.Instance.Info($"Initiating immediate server shutdown: {reason}");

                // Execute the shutdown command asynchronously
                var tcs = new TaskCompletionSource<string>();
                var cr = new CommandResult(shutdownCommand, tcs);
                SdtdConsole.Instance.ExecuteAsync(shutdownCommand, cr);
                string result = await tcs.Task;

                LogService.Instance.Info($"Shutdown command executed successfully. Server will shutdown immediately");

                // Return null payload as per Takaro specification
                WebSocketMessage message = WebSocketMessage.CreateResponse(requestId, null);
                SendMessage(message);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error executing shutdown command: {ex.Message}");
                SendErrorResponse(requestId, $"Failed to initiate shutdown: {ex.Message}");
            }
        }

        #endregion

        #region Event Handlers
        public void SendGameEvent(string type, object data)
        {
            if (data == null)
                return;

            WebSocketMessage message = WebSocketMessage.Create(
                WebSocketMessage.MessageTypes.GameEvent,
                new Dictionary<string, object> { 
                    { "type", type },
                    { "data", data }
                 }
            );

            SendMessage(message);
        }

        // Public methods to send game events
        public void SendPlayerConnected(ClientInfo cInfo)
        {
            if (cInfo == null) return;

            SendGameEvent(
                "player-connected",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                }
            );
        }

        public void SendPlayerDisconnected(ClientInfo cInfo)
        {
            if (cInfo == null)
                return;

        SendGameEvent(
                "player-disconnected",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                }
            );
        }

        public void SendChatMessage(ClientInfo cInfo, EChatType type, int _senderId, string msg, List<int> recipientEntityIds)
        {
            if (cInfo == null) return;

            string channel = "unknown";

            switch (type)
            {
                case EChatType.Global:
                    channel = "global";
                    break;
                case EChatType.Whisper:
                    channel = "whisper";
                    break;
                case EChatType.Friends:
                    channel = "friends";
                    break;
                case EChatType.Party:
                    channel = "team";
                    break;                    
                default:
                    channel = "unknown";
                    break;
            }

            SendGameEvent(
                "chat-message",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                    { "msg", msg },
                    { "channel", channel }
                }
            );
        }

        public void SendEntityKilled(ClientInfo killerInfo, string entityName, string entityType, string weapon = null)
        {
            if (killerInfo == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", Shared.TransformClientInfoToTakaroPlayer(killerInfo) },
                { "entity", entityType }
            };

            // Add weapon information if available
            if (!string.IsNullOrEmpty(weapon))
            {
                eventData["weapon"] = weapon;
            }

            SendGameEvent("entity-killed", eventData);
        }

        public void SendPlayerDeath(ClientInfo deadPlayerInfo, ClientInfo attackerInfo, Vector3 deathPosition)
        {
            if (deadPlayerInfo == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", Shared.TransformClientInfoToTakaroPlayer(deadPlayerInfo) },
                { "position", new Dictionary<string, object>
                    {
                        { "x", deathPosition.x },
                        { "y", deathPosition.y },
                        { "z", deathPosition.z }
                    }
                }
            };

            // Add attacker information if available
            if (attackerInfo != null)
            {
                eventData["attacker"] = Shared.TransformClientInfoToTakaroPlayer(attackerInfo);
            }

            SendGameEvent("player-death", eventData);
        }

        public void SendLogEvent(string logMessage)
        {
            if (string.IsNullOrEmpty(logMessage))
                return;

            SendGameEvent("log", new Dictionary<string, object>
            {
                { "msg", logMessage }
            });
        }

        #endregion
    }
}