using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Oxide.Core;
using UnityEngine;

namespace Oxide.Plugins
{
    [Info("TakaroConnector", "Takaro", "1.0.0")]
    [Description("Takaro Generic Connector — connects outbound to Takaro via WebSocket")]
    public class TakaroConnector : RustPlugin
    {
        // --- Configuration ---

        private string _wsUrl;
        private string _registrationToken;
        private string _identityToken;
        private bool _debug;

        private const long InitialReconnectDelay = 5000;
        private const long MaxReconnectDelay = 300000;
        private const double BackoffMultiplier = 1.5;

        // --- WebSocket State ---

        private ClientWebSocket _ws;
        private CancellationTokenSource _cts;
        private volatile bool _shouldReconnect = true;
        private long _currentReconnectDelay = InitialReconnectDelay;
        private volatile bool _connected;
        private readonly object _sendLock = new object();
        private readonly Dictionary<string, Vector3> _lastPosition = new Dictionary<string, Vector3>();

        // --- Lifecycle ---

        private void Init()
        {
            _wsUrl = Environment.GetEnvironmentVariable("TAKARO_WS_URL") ?? "wss://connect.takaro.io/";
            _registrationToken = Environment.GetEnvironmentVariable("TAKARO_REGISTRATION_TOKEN") ?? "";
            _identityToken = Environment.GetEnvironmentVariable("TAKARO_IDENTITY_TOKEN") ?? "";
            _debug = Environment.GetEnvironmentVariable("TAKARO_DEBUG")?.ToLower() == "true";

            if (string.IsNullOrEmpty(_registrationToken))
            {
                PrintWarning("TAKARO_REGISTRATION_TOKEN not set. Plugin will not connect.");
                return;
            }

            Subscribe("OnPlayerConnected");
            Subscribe("OnPlayerDisconnected");
            Subscribe("OnPlayerChat");
            Subscribe("OnPlayerDeath");
            Subscribe("OnEntityDeath");
            Subscribe("OnServerMessage");

            LogInfo($"Connecting to {_wsUrl}");
            StartConnection();
        }

        private void Unload()
        {
            _shouldReconnect = false;
            _connected = false;
            _cts?.Cancel();
            try { _ws?.Dispose(); } catch { }
            _lastPosition.Clear();
        }

        // --- WebSocket Connection ---

        private void StartConnection()
        {
            _cts?.Cancel();
            _cts = new CancellationTokenSource();
            var token = _cts.Token;

            Task.Run(async () =>
            {
                try
                {
                    _ws = new ClientWebSocket();
                    await _ws.ConnectAsync(new Uri(_wsUrl), token);
                    LogInfo("WebSocket connected");
                    await ReceiveLoop(token);
                }
                catch (OperationCanceledException) { }
                catch (Exception ex)
                {
                    LogWarning($"WebSocket connection failed: {ex.Message}");
                }
                finally
                {
                    _connected = false;
                    try { _ws?.Dispose(); } catch { }
                    _ws = null;

                    if (_shouldReconnect)
                        ScheduleReconnect();
                }
            });
        }

        private async Task ReceiveLoop(CancellationToken token)
        {
            var buffer = new byte[8192];

            while (_ws?.State == WebSocketState.Open && !token.IsCancellationRequested)
            {
                var sb = new StringBuilder();
                WebSocketReceiveResult result;

                do
                {
                    result = await _ws.ReceiveAsync(new ArraySegment<byte>(buffer), token);
                    sb.Append(Encoding.UTF8.GetString(buffer, 0, result.Count));
                } while (!result.EndOfMessage);

                if (result.MessageType == WebSocketMessageType.Close)
                {
                    var code = (int)(result.CloseStatus ?? WebSocketCloseStatus.NormalClosure);
                    var reason = result.CloseStatusDescription ?? "";
                    LogInfo($"WebSocket closed (code={code}, reason={reason})");

                    if (code == 1008 || code == 4001 || code == 4003)
                    {
                        LogWarning("Authentication error, disabling reconnect");
                        _shouldReconnect = false;
                    }
                    break;
                }

                var message = sb.ToString();
                if (_debug)
                    LogDebug($"WS RECV: {message}");

                try
                {
                    OnWsMessage(message);
                }
                catch (Exception ex)
                {
                    LogWarning($"Error handling message: {ex.Message}");
                }
            }
        }

        private void OnWsMessage(string message)
        {
            var json = JObject.Parse(message);
            var type = json.Value<string>("type") ?? "";

            switch (type)
            {
                case "connected":
                    LogInfo("Received server hello, sending identify...");
                    SendIdentify();
                    break;

                case "identifyResponse":
                    HandleIdentifyResponse(json);
                    break;

                case "request":
                    HandleRequest(json);
                    break;

                case "error":
                    var errorMsg = json["payload"]?.Value<string>("message")
                                   ?? json.Value<string>("message")
                                   ?? "unknown";
                    var reqId = json.Value<string>("requestId");
                    LogWarning($"Server error: {errorMsg}" + (reqId != null ? $" (requestId={reqId})" : ""));
                    break;

                default:
                    LogWarning($"Unknown message type: {type}");
                    break;
            }
        }

        // --- Reconnection ---

        private void ScheduleReconnect()
        {
            if (!_shouldReconnect) return;

            var delaySec = _currentReconnectDelay / 1000.0f;
            LogInfo($"Reconnecting in {delaySec:F0}s...");

            timer.In(delaySec, () =>
            {
                if (_shouldReconnect)
                    StartConnection();
            });

            _currentReconnectDelay = Math.Min(
                (long)(_currentReconnectDelay * BackoffMultiplier),
                MaxReconnectDelay
            );
        }

        // --- Send Helpers ---

        private void WsSend(string message)
        {
            var ws = _ws;
            if (ws?.State != WebSocketState.Open) return;

            if (_debug && !message.Contains("\"type\":\"identify\""))
                LogDebug($"WS SEND: {message}");

            var bytes = Encoding.UTF8.GetBytes(message);
            _ = Task.Run(() =>
            {
                lock (_sendLock)
                {
                    try
                    {
                        ws.SendAsync(
                            new ArraySegment<byte>(bytes),
                            WebSocketMessageType.Text,
                            true,
                            _cts?.Token ?? CancellationToken.None
                        ).GetAwaiter().GetResult();
                    }
                    catch (Exception ex)
                    {
                        LogWarning($"Send failed: {ex.Message}");
                    }
                }
            });
        }

        private void SendIdentify()
        {
            var msg = new JObject
            {
                ["type"] = "identify",
                ["payload"] = new JObject
                {
                    ["identityToken"] = _identityToken ?? "",
                    ["registrationToken"] = _registrationToken ?? ""
                }
            };
            LogDebug("WS SEND identify (tokens redacted)");
            WsSend(msg.ToString(Formatting.None));
        }

        private void HandleIdentifyResponse(JObject json)
        {
            var payload = json["payload"] as JObject ?? new JObject();

            var error = payload["error"];
            if (error != null && error.Type != JTokenType.Null)
            {
                var errorMessage = error.Type == JTokenType.Object
                    ? error.Value<string>("message") ?? error.ToString()
                    : error.ToString();
                LogWarning($"Identify failed: {errorMessage}");
                return;
            }

            _connected = true;
            _currentReconnectDelay = InitialReconnectDelay;

            var serverId = payload["gameServerId"]?.Value<string>()
                           ?? payload["server"]?.Value<string>("id");
            if (serverId != null)
                LogInfo($"Identified and connected, server ID: {serverId}");
            else
                LogInfo("Identified and connected");
        }

        private void SendResponse(string requestId, JToken payload, string error)
        {
            var msg = new JObject
            {
                ["type"] = "response",
                ["requestId"] = requestId
            };

            if (error != null)
                msg["error"] = error;
            else if (payload != null)
                msg["payload"] = payload;

            if (_debug)
                LogDebug($"WS SEND response (requestId={requestId})");

            WsSend(msg.ToString(Formatting.None));
        }

        private void SendGameEvent(string eventType, JObject data)
        {
            if (!_connected) return;

            var msg = new JObject
            {
                ["type"] = "gameEvent",
                ["payload"] = new JObject
                {
                    ["type"] = eventType,
                    ["data"] = data
                }
            };

            if (_debug)
                LogDebug($"WS SEND gameEvent: {eventType}");

            WsSend(msg.ToString(Formatting.None));
        }

        // --- Request Handling ---

        private void HandleRequest(JObject json)
        {
            var requestId = json.Value<string>("requestId");
            if (requestId == null)
            {
                LogWarning("Received request without requestId");
                return;
            }

            var payload = json["payload"] as JObject ?? new JObject();
            var action = payload.Value<string>("action") ?? "";
            if (_debug)
                LogDebug($"Request: action={action}, requestId={requestId}");

            JObject args;
            var argsToken = payload["args"];
            try
            {
                if (argsToken != null && argsToken.Type == JTokenType.String)
                {
                    var argsStr = argsToken.Value<string>();
                    args = string.IsNullOrEmpty(argsStr) ? new JObject() : JObject.Parse(argsStr);
                }
                else if (argsToken != null && argsToken.Type == JTokenType.Object)
                {
                    args = (JObject)argsToken;
                }
                else
                {
                    args = new JObject();
                }
            }
            catch (Exception ex)
            {
                SendResponse(requestId, null, $"Invalid args JSON: {ex.Message}");
                return;
            }

            switch (action)
            {
                case "testReachability":
                    SendResponse(requestId, new JObject { ["connectable"] = true, ["reason"] = null }, null);
                    break;

                case "getPlayer":
                case "getPlayers":
                case "getPlayerLocation":
                case "getPlayerInventory":
                case "listItems":
                case "listEntities":
                case "listLocations":
                case "executeConsoleCommand":
                case "sendMessage":
                case "giveItem":
                case "teleportPlayer":
                case "kickPlayer":
                case "banPlayer":
                case "unbanPlayer":
                case "listBans":
                case "shutdown":
                    RunOnMainThread(requestId, action, args);
                    break;

                default:
                    SendResponse(requestId, null, $"Action not implemented: {action}");
                    break;
            }
        }

        private void RunOnMainThread(string requestId, string action, JObject args)
        {
            NextTick(() =>
            {
                try
                {
                    var result = ExecuteAction(action, args);
                    SendResponse(requestId, result, null);
                }
                catch (Exception ex)
                {
                    LogWarning($"Action {action} failed: {ex.Message}");
                    SendResponse(requestId, null, ex.Message);
                }
            });
        }

        private JToken ExecuteAction(string action, JObject args)
        {
            switch (action)
            {
                case "getPlayer": return HandleGetPlayer(args);
                case "getPlayers": return HandleGetPlayers();
                case "getPlayerLocation": return HandleGetPlayerLocation(args);
                case "getPlayerInventory": return HandleGetPlayerInventory(args);
                case "listItems": return HandleListItems();
                case "listEntities": return HandleListEntities();
                case "listLocations": return HandleListLocations();
                case "executeConsoleCommand": return HandleExecuteConsoleCommand(args);
                case "sendMessage": HandleSendMessage(args); return new JObject();
                case "giveItem": HandleGiveItem(args); return new JObject();
                case "teleportPlayer": HandleTeleportPlayer(args); return new JObject();
                case "kickPlayer": HandleKickPlayer(args); return new JObject();
                case "banPlayer": HandleBanPlayer(args); return new JObject();
                case "unbanPlayer": HandleUnbanPlayer(args); return new JObject();
                case "listBans": return HandleListBans();
                case "shutdown": HandleShutdown(); return new JObject();
                default: throw new Exception($"Unknown action: {action}");
            }
        }

        // --- Action Handlers ---

        private BasePlayer FindPlayerByGameId(string gameId)
        {
            if (string.IsNullOrEmpty(gameId)) return null;

            if (ulong.TryParse(gameId, out var steamId))
                return BasePlayer.FindByID(steamId) ?? BasePlayer.FindSleeping(steamId);

            return null;
        }

        private JObject PlayerToJson(BasePlayer player)
        {
            var steamId = player.UserIDString;
            return new JObject
            {
                ["gameId"] = steamId,
                ["name"] = player.displayName,
                ["steamId"] = steamId,
                ["epicOnlineServicesId"] = "",
                ["xboxLiveId"] = "",
                ["platformId"] = $"steam:{steamId}",
                ["ip"] = player.net?.connection?.ipaddress?.Split(':')[0] ?? "",
                ["ping"] = player.IsConnected ? Network.Net.sv.GetAveragePing(player.net.connection) : 0
            };
        }

        private JToken HandleGetPlayer(JObject args)
        {
            var gameId = args.Value<string>("gameId");
            var player = FindPlayerByGameId(gameId);
            return player != null ? (JToken)PlayerToJson(player) : JValue.CreateNull();
        }

        private JToken HandleGetPlayers()
        {
            var arr = new JArray();
            foreach (var player in BasePlayer.activePlayerList)
                arr.Add(PlayerToJson(player));
            return arr;
        }

        private JToken HandleGetPlayerLocation(JObject args)
        {
            var gameId = args.Value<string>("gameId");
            var player = FindPlayerByGameId(gameId);

            if (player != null)
            {
                var pos = player.transform.position;
                _lastPosition[gameId] = pos;
                return new JObject
                {
                    ["x"] = pos.x,
                    ["y"] = pos.y,
                    ["z"] = pos.z
                };
            }

            if (_lastPosition.TryGetValue(gameId, out var cached))
            {
                return new JObject
                {
                    ["x"] = cached.x,
                    ["y"] = cached.y,
                    ["z"] = cached.z
                };
            }

            return new JObject { ["x"] = 0, ["y"] = 0, ["z"] = 0 };
        }

        private JToken HandleGetPlayerInventory(JObject args)
        {
            var gameId = args.Value<string>("gameId");
            var player = FindPlayerByGameId(gameId);
            if (player == null) return JValue.CreateNull();

            var items = new JArray();
            var containers = new[] {
                player.inventory.containerMain,
                player.inventory.containerBelt,
                player.inventory.containerWear
            };

            foreach (var container in containers)
            {
                if (container == null) continue;
                foreach (var item in container.itemList)
                {
                    items.Add(new JObject
                    {
                        ["code"] = item.info.shortname,
                        ["name"] = item.info.displayName.english,
                        ["amount"] = item.amount,
                        ["quality"] = ""
                    });
                }
            }
            return items;
        }

        private JToken HandleListItems()
        {
            var arr = new JArray();
            foreach (var def in ItemManager.itemList)
            {
                arr.Add(new JObject
                {
                    ["code"] = def.shortname,
                    ["name"] = def.displayName.english,
                    ["description"] = def.displayDescription?.english ?? ""
                });
            }
            return arr;
        }

        private JToken HandleListEntities()
        {
            var seen = new HashSet<string>();
            var arr = new JArray();

            foreach (var path in GameManifest.Current.entities)
            {
                if (path.Contains("corpse") || path.Contains("ragdoll") || path.Contains("_dead")) continue;

                var prefab = GameManager.server.FindPrefab(path);
                if (prefab == null) continue;

                if (prefab.GetComponent<BaseNpc>() == null && prefab.GetComponent<NPCPlayer>() == null) continue;

                var shortName = prefab.name;
                if (string.IsNullOrEmpty(shortName) || !seen.Add(shortName)) continue;

                arr.Add(new JObject
                {
                    ["code"] = shortName,
                    ["name"] = shortName,
                    ["description"] = ""
                });
            }

            return arr;
        }

        private JToken HandleListLocations()
        {
            var arr = new JArray();
            if (TerrainMeta.Path?.Monuments != null)
            {
                foreach (var monument in TerrainMeta.Path.Monuments)
                {
                    if (monument == null) continue;
                    var pos = monument.transform.position;
                    arr.Add(new JObject
                    {
                        ["name"] = monument.displayPhrase?.english ?? monument.name,
                        ["code"] = monument.name,
                        ["position"] = new JObject
                        {
                            ["x"] = pos.x,
                            ["y"] = pos.y,
                            ["z"] = pos.z
                        }
                    });
                }
            }
            return arr;
        }

        private JToken HandleExecuteConsoleCommand(JObject args)
        {
            var command = args.Value<string>("command") ?? "";
            try
            {
                var result = ConsoleSystem.Run(ConsoleSystem.Option.Server, command);
                return new JObject
                {
                    ["success"] = true,
                    ["rawResult"] = result ?? "",
                    ["errorMessage"] = ""
                };
            }
            catch (Exception ex)
            {
                return new JObject
                {
                    ["success"] = false,
                    ["rawResult"] = "",
                    ["errorMessage"] = ex.Message
                };
            }
        }

        private void HandleSendMessage(JObject args)
        {
            var message = args.Value<string>("message") ?? "";
            string recipientGameId = null;

            var opts = args["opts"] as JObject;
            var recipient = opts?["recipient"] as JObject;
            recipientGameId = recipient?.Value<string>("gameId");

            if (!string.IsNullOrEmpty(recipientGameId))
            {
                var player = FindPlayerByGameId(recipientGameId);
                player?.ChatMessage(message);
            }
            else
            {
                ConsoleNetwork.BroadcastToAllClients("chat.add", 2, 0, message);
            }
        }

        private void HandleGiveItem(JObject args)
        {
            var playerObj = args["player"] as JObject;
            var gameId = playerObj?.Value<string>("gameId");
            var itemCode = args.Value<string>("item");
            var amount = args.Value<int?>("amount") ?? 1;

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new Exception("Player not found");

            var itemDef = ItemManager.FindItemDefinition(itemCode);
            if (itemDef == null) throw new Exception($"Item not found: {itemCode}");

            var item = ItemManager.Create(itemDef, amount);
            if (!player.inventory.GiveItem(item))
            {
                item.Drop(player.transform.position, Vector3.up);
            }
        }

        private void HandleTeleportPlayer(JObject args)
        {
            var playerObj = args["player"] as JObject;
            var gameId = playerObj?.Value<string>("gameId");
            var x = args.Value<float?>("x") ?? 0;
            var y = args.Value<float?>("y") ?? 0;
            var z = args.Value<float?>("z") ?? 0;

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new Exception("Player not found");

            player.Teleport(new Vector3(x, y, z));
        }

        private void HandleKickPlayer(JObject args)
        {
            var playerObj = args["player"] as JObject;
            var gameId = playerObj?.Value<string>("gameId");
            var reason = args.Value<string>("reason") ?? "";

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new Exception("Player not found");

            player.Kick(reason);
        }

        private void HandleBanPlayer(JObject args)
        {
            var playerObj = args["player"] as JObject;
            var gameId = playerObj?.Value<string>("gameId");
            var reason = args.Value<string>("reason") ?? "";

            if (string.IsNullOrEmpty(gameId)) throw new Exception("gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new Exception("Invalid gameId");

            var player = FindPlayerByGameId(gameId);
            var name = player?.displayName ?? gameId;

            ServerUsers.Set(steamId, ServerUsers.UserGroup.Banned, name, reason);
            ServerUsers.Save();

            player?.Kick($"Banned: {reason}");
        }

        private void HandleUnbanPlayer(JObject args)
        {
            var gameId = args.Value<string>("gameId");
            if (string.IsNullOrEmpty(gameId)) throw new Exception("gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new Exception("Invalid gameId");

            ServerUsers.Remove(steamId);
            ServerUsers.Save();
        }

        private JToken HandleListBans()
        {
            var arr = new JArray();
            var bans = ServerUsers.GetAll(ServerUsers.UserGroup.Banned);
            foreach (var ban in bans)
            {
                arr.Add(new JObject
                {
                    ["player"] = new JObject
                    {
                        ["gameId"] = ban.steamid.ToString(),
                        ["name"] = ban.username ?? ""
                    },
                    ["reason"] = ban.notes ?? "",
                    ["expiresAt"] = null
                });
            }
            return arr;
        }

        private void HandleShutdown()
        {
            ConsoleSystem.Run(ConsoleSystem.Option.Server, "quit");
        }

        // --- Game Event Hooks ---

        private void OnPlayerConnected(BasePlayer player)
        {
            if (player == null) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-connected", data);
        }

        private void OnPlayerDisconnected(BasePlayer player, string reason)
        {
            if (player == null) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-disconnected", data);
        }

        private object OnPlayerChat(BasePlayer player, string message, ConVar.Chat.ChatChannel channel)
        {
            if (player == null) return null;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player),
                ["channel"] = channel.ToString().ToLower(),
                ["msg"] = message
            };
            SendGameEvent("chat-message", data);
            return null;
        }

        private void OnPlayerDeath(BasePlayer player, HitInfo info)
        {
            if (player == null) return;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };

            var attacker = info?.InitiatorPlayer;
            if (attacker != null && attacker != player)
            {
                data["attacker"] = PlayerToJson(attacker);
            }

            var pos = player.transform.position;
            data["position"] = new JObject
            {
                ["x"] = pos.x,
                ["y"] = pos.y,
                ["z"] = pos.z
            };

            SendGameEvent("player-death", data);
        }

        private void OnEntityDeath(BaseCombatEntity entity, HitInfo info)
        {
            if (entity == null || entity is BasePlayer) return;

            var attacker = info?.InitiatorPlayer;
            if (attacker == null) return;

            var weapon = info?.Weapon?.GetItem()?.info?.shortname
                         ?? info?.WeaponPrefab?.ShortPrefabName
                         ?? "";
            var data = new JObject
            {
                ["player"] = PlayerToJson(attacker),
                ["entity"] = entity.ShortPrefabName ?? entity.GetType().Name,
                ["weapon"] = weapon
            };
            SendGameEvent("entity-killed", data);
        }

        private void OnServerMessage(string message, string username, string color, ulong userid)
        {
            if (string.IsNullOrEmpty(message)) return;
            if (message.StartsWith("%.") || message.StartsWith("[event]")) return;
            if (message.StartsWith("[Takaro") || message.StartsWith("[Carbon]")) return;
            if (message.StartsWith("SteamServer") || message.StartsWith("Saving complete")) return;

            var data = new JObject
            {
                ["msg"] = message
            };
            SendGameEvent("log", data);
        }

        // --- Logging Helpers ---

        private void LogInfo(string message) => Puts($"[Takaro] {message}");
        private void LogWarning(string message) => PrintWarning($"[Takaro] {message}");
        private void LogDebug(string message) { if (_debug) Puts($"[Takaro DEBUG] {message}"); }
    }
}
