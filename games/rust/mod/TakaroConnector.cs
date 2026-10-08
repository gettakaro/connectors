using System;
using System.Collections.Generic;
using System.Globalization;
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
    [Info("TakaroConnector", "Takaro", "0.1.3")] // x-release-please-version
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
        private volatile bool _identifySent;
        private readonly object _sendLock = new object();
        private readonly Dictionary<string, Vector3> _lastPosition = new Dictionary<string, Vector3>();

        // --- Timed Bans ---
        //
        // Rust's native ban list (ServerUsers) has no expiry, so timed bans are kept
        // in the plugin's own data file and enforced on login. Permanent bans still
        // go through the native ban list.

        private const string BanDataFile = "TakaroConnector_bans";
        private const float BanSweepInterval = 60f;

        private readonly Dictionary<ulong, TimedBan> _timedBans = new Dictionary<ulong, TimedBan>();
        private Timer _banSweepTimer;

        private class TimedBan
        {
            [JsonProperty("steamId")]
            public string SteamId { get; set; }

            [JsonProperty("name")]
            public string Name { get; set; }

            [JsonProperty("reason")]
            public string Reason { get; set; }

            [JsonProperty("expiresAt")]
            public string ExpiresAt { get; set; }
        }

        // --- Lifecycle ---

        private void Init()
        {
            LoadTimedBans();
            _banSweepTimer = timer.Every(BanSweepInterval, () => PruneExpiredBans());

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
            _banSweepTimer?.Destroy();
            _banSweepTimer = null;
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
                    _identifySent = false;
                    LogInfo("WebSocket connected");
                    // Identify is the first frame this connector sends, the way every other
                    // Takaro connector in this repository does it. Waiting to be greeted
                    // instead means a peer that expects the client to speak first never
                    // hears from this server at all: the socket sits open and silent.
                    SendIdentify();
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
                    LogDebug($"WS RECV: {FrameSummary(message)}");

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

        // takaro:frames-begin
        private static string FrameSummary(string message)
        {
            try
            {
                var json = JObject.Parse(message);
                var type = json.Value<string>("type") ?? "";
                if (type != "request") return $"type={type}";
                return $"type=request action={json.Value<string>("action") ?? ""} " +
                    $"requestId={json.Value<string>("requestId") ?? ""}";
            }
            catch
            {
                return $"unparseable frame ({Encoding.UTF8.GetByteCount(message ?? "")} bytes)";
            }
        }
        // takaro:frames-end

        // takaro:identity-begin
        // A real Rust player's UserIDString is a SteamID64. NPCs (scientists, murderers, tunnel
        // dwellers) are BasePlayers too, with small numeric ids; sending one of those as a
        // Takaro player creates a bogus profile ("Scientist", steamId "67").
        private static bool IsSteamPlayerId(string id)
        {
            if (string.IsNullOrEmpty(id) || id.Length != 17 || !id.StartsWith("7656119", StringComparison.Ordinal))
                return false;
            foreach (var c in id)
                if (c < '0' || c > '9') return false;
            return true;
        }

        // Takaro matches players only on steamId / epicOnlineServicesId / xboxLiveId /
        // platformId and never derives steamId from platformId, so steamId is always sent
        // raw. Rust is Steam-only: no EOS or Xbox id exists, and those keys are omitted.
        private static string[][] IdentityFields(string steamId)
        {
            return new[]
            {
                new[] { "gameId", steamId },
                new[] { "steamId", steamId },
                new[] { "platformId", "steam:" + steamId },
            };
        }
        // takaro:identity-end

        private static bool IsRealPlayer(BasePlayer player)
        {
            return player != null && !player.IsNpc && IsSteamPlayerId(player.UserIDString);
        }

        private static JObject IdentityJson(string steamId, string name)
        {
            var json = new JObject();
            foreach (var field in IdentityFields(steamId))
                json[field[0]] = field[1];
            json["name"] = name ?? "";
            return json;
        }

        private void OnWsMessage(string message)
        {
            var json = JObject.Parse(message);
            var type = json.Value<string>("type") ?? "";

            switch (type)
            {
                case "connected":
                    // Takaro greets a fresh connection. Identify has already gone out when
                    // the socket opened, so this only does anything if the greeting beat it.
                    LogInfo("Received server hello");
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
            // At most once per connection: the socket opening and a server greeting both
            // lead here, and a second identify would look like a second session.
            if (_identifySent) return;
            _identifySent = true;

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
                LogInfo($"Identified successfully, server ID: {serverId}");
            else
                LogInfo("Identified successfully");
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

        // Takaro sends the player either flat ({gameId}) or nested ({player:{gameId}}),
        // depending on the route, so every handler accepts both.
        private static string GameIdFrom(JObject args)
        {
            var nested = (args["player"] as JObject)?.Value<string>("gameId");
            return string.IsNullOrEmpty(nested) ? args.Value<string>("gameId") : nested;
        }

        private BasePlayer FindPlayerByGameId(string gameId)
        {
            if (string.IsNullOrEmpty(gameId)) return null;

            if (ulong.TryParse(gameId, out var steamId))
                return BasePlayer.FindByID(steamId) ?? BasePlayer.FindSleeping(steamId);

            return null;
        }

        private JObject PlayerToJson(BasePlayer player)
        {
            var json = IdentityJson(player.UserIDString, player.displayName);
            json["ip"] = player.net?.connection?.ipaddress?.Split(':')[0] ?? "";
            json["ping"] = player.IsConnected ? Network.Net.sv.GetAveragePing(player.net.connection) : 0;
            return json;
        }

        private JToken HandleGetPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var player = FindPlayerByGameId(gameId);
            return player != null ? (JToken)PlayerToJson(player) : JValue.CreateNull();
        }

        private JToken HandleGetPlayers()
        {
            var arr = new JArray();
            foreach (var player in BasePlayer.activePlayerList)
                if (IsRealPlayer(player)) arr.Add(PlayerToJson(player));
            return arr;
        }

        private JToken HandleGetPlayerLocation(JObject args)
        {
            var gameId = GameIdFrom(args);
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
            var gameId = GameIdFrom(args);
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
                    ["name"] = EntityNames.EntityDisplayName(shortName),
                    ["description"] = ""
                });
            }

            return arr;
        }

        // What a console command looks like in the log: the verb, and how many arguments
        // came with it. `console: help` is unchanged; `say <anything>` never appears.
        private static string CommandSummary(string command)
        {
            var parts = (command ?? "").Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return "";
            return parts.Length == 1 ? parts[0] : parts[0] + " (" + (parts.Length - 1) + " args)";
        }

        // takaro:names-begin
        // Entity prefabs have no localised display name the way items do -- `bear`,
        // `scientistnpc_heavy`, `wolf2` is all the server knows them by. Opening the
        // separators and capitalising put "Scientistnpc Heavy" and "Missionprovider
        // Floatingcity A" in the one field Takaro shows a
        // human: a formatted dev code, not a name.
        //
        // The table is the prefab list a real server returned; anything outside it goes
        // through the rules below it. Nothing in this region touches Rust, Carbon or
        // Unity, and the markers are what `tests/names/run.sh` lifts out to compile and
        // run it on its own -- the behaviour is proven there, not grepped for.
        private static class EntityNames
        {
            private static readonly Dictionary<string, string> Table =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "scientistnpc_arena", "Arena Scientist" },
                    { "scientistnpc_bradley", "Bradley Scientist" },
                    { "scientistnpc_bradley_heavy", "Bradley Heavy Scientist" },
                    { "scientistnpc_cargo", "Cargo Ship Scientist" },
                    { "scientistnpc_cargo_turret_any", "Cargo Ship Turret Scientist" },
                    { "scientistnpc_cargo_turret_lr300", "Cargo Ship Turret Scientist (LR-300)" },
                    { "scientistnpc_ch47_gunner", "Chinook Gunner Scientist" },
                    { "scientistnpc_excavator", "Excavator Scientist" },
                    { "scientistnpc_full_any", "Scientist" },
                    { "scientistnpc_full_lr300", "Scientist (LR-300)" },
                    { "scientistnpc_full_mp5", "Scientist (MP5)" },
                    { "scientistnpc_full_pistol", "Scientist (Pistol)" },
                    { "scientistnpc_full_shotgun", "Scientist (Shotgun)" },
                    { "scientistnpc_heavy", "Heavy Scientist" },
                    { "scientistnpc_junkpile_pistol", "Junkpile Scientist" },
                    { "scientistnpc_oilrig", "Oil Rig Scientist" },
                    { "scientistnpc_outbreak", "Outbreak Scientist" },
                    { "scientistnpc_patrol", "Patrol Scientist" },
                    { "scientistnpc_patrol_arctic", "Arctic Patrol Scientist" },
                    { "scientistnpc_peacekeeper", "Peacekeeper Scientist" },
                    { "scientistnpc_ptboat", "Patrol Boat Scientist" },
                    { "scientistnpc_rhib", "RHIB Scientist" },
                    { "scientistnpc_roam", "Roaming Scientist" },
                    { "scientistnpc_roam_nvg_variant", "Roaming Scientist (Night Vision)" },
                    { "scientistnpc_roamtethered", "Tethered Roaming Scientist" },
                    { "npc_bandit_guard", "Bandit Guard" },
                    { "npc_tunneldweller", "Tunnel Dweller" },
                    { "npc_tunneldwellerspawned", "Tunnel Dweller (Spawned)" },
                    { "npc_underwaterdweller", "Underwater Dweller" },
                    { "npcplayertest", "NPC Player (Test)" },
                    { "polarbear", "Polar Bear" },
                    { "bear", "Bear" },
                    { "bear_tutorial", "Bear (Tutorial)" },
                    { "boar", "Boar" },
                    { "chicken", "Chicken" },
                    { "chicken.tutorial", "Chicken (Tutorial)" },
                    { "stag", "Stag" },
                    { "wolf", "Wolf" },
                    { "wolf2", "Wolf" },
                    { "ridablehorse", "Horse" },
                    { "ridablehorse2", "Horse" },
                    { "simpleshark", "Shark" },
                    { "shark_unused", "Shark (Unused)" },
                    { "zombie", "Zombie" },
                    { "scarecrow", "Scarecrow" },
                    { "scarecrow_dungeon", "Scarecrow (Dungeon)" },
                    { "scarecrow_dungeonnoroam", "Scarecrow (Dungeon, Stationary)" },
                    { "gingerbread_dungeon", "Gingerbread Man (Dungeon)" },
                    { "gingerbread_meleedungeon", "Gingerbread Man (Melee Dungeon)" },
                    { "frankensteinpet", "Frankenstein Pet" },
                    { "apartment_vendor", "Apartment Vendor" },
                    { "apartment_security", "Apartment Security" },
                    { "farm_access_guard", "Farm Access Guard" },
                    { "bandit_conversationalist", "Bandit Conversationalist" },
                    { "bandit_shopkeeper", "Bandit Shopkeeper" },
                    { "bandit_shopkeeper_sitting", "Bandit Shopkeeper (Sitting)" },
                    { "boat_shopkeeper", "Boat Shopkeeper" },
                    { "stables_shopkeeper", "Stables Shopkeeper" },
                    { "livestockvendor_stables", "Stables Livestock Vendor" },
                    { "waterwell_shopkeeper", "Water Well Shopkeeper" },
                    { "missionprovider_bandit_a", "Bandit Mission Provider A" },
                    { "missionprovider_bandit_b", "Bandit Mission Provider B" },
                    { "missionprovider_fishing_a", "Fishing Mission Provider A" },
                    { "missionprovider_fishing_b", "Fishing Mission Provider B" },
                    { "missionprovider_floatingcity_a", "Floating City Mission Provider A" },
                    { "missionprovider_generic_a", "Generic Mission Provider A" },
                    { "missionprovider_outpost_a", "Outpost Mission Provider A" },
                    { "missionprovider_outpost_b", "Outpost Mission Provider B" },
                    { "missionprovider_stables_a", "Stables Mission Provider A" },
                    { "missionprovider_stables_b", "Stables Mission Provider B" },
                    { "missionprovider_test", "Mission Provider (Test)" },
                    { "missionprovider_tutorial", "Mission Provider (Tutorial)" }
                };

            // Prefab words that are several words glued together, or an abbreviation that
            // capitalising alone would mangle into `Lr300`, `Ch47`, `Mp5`.
            private static readonly Dictionary<string, string> Glued =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "scientistnpc", "Scientist" },
                    { "tunneldweller", "Tunnel Dweller" },
                    { "underwaterdweller", "Underwater Dweller" },
                    { "polarbear", "Polar Bear" },
                    { "ridablehorse", "Horse" },
                    { "simpleshark", "Shark" },
                    { "waterwell", "Water Well" },
                    { "missionprovider", "Mission Provider" },
                    { "meleedungeon", "Melee Dungeon" },
                    { "dungeonnoroam", "Dungeon, Stationary" },
                    { "floatingcity", "Floating City" },
                    { "lr300", "LR-300" },
                    { "mp5", "MP5" },
                    { "rhib", "RHIB" },
                    { "ch47", "CH-47" },
                    { "nvg", "Night Vision" },
                    { "ptboat", "Patrol Boat" },
                    { "oilrig", "Oil Rig" },
                    { "npc", "NPC" }
                };

            // Not what the thing is but which copy of it, so these become a parenthesised
            // suffix rather than a word in the middle of the name.
            private static readonly Dictionary<string, string> Variants =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "tutorial", "Tutorial" },
                    { "unused", "Unused" },
                    { "test", "Test" },
                    { "spawned", "Spawned" }
                };

            private static readonly char[] Separators = { '_', '-', '.' };

            public static string EntityDisplayName(string shortName)
            {
                if (string.IsNullOrEmpty(shortName)) return shortName;

                string known;
                if (Table.TryGetValue(shortName, out known)) return known;

                var parts = shortName.Split(Separators, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) return shortName;

                // `missionprovider_bandit_a` is the bandit camp's mission provider, variant
                // A: the role belongs between the place and the variant, not at the front.
                var provider = parts[0].Equals("missionprovider", StringComparison.OrdinalIgnoreCase);
                var words = new List<string>(parts.Length);
                var suffixes = new List<string>();

                for (var i = provider ? 1 : 0; i < parts.Length; i++)
                {
                    string variant;
                    if (Variants.TryGetValue(parts[i], out variant))
                    {
                        suffixes.Add(variant);
                        continue;
                    }
                    words.Add(Word(parts[i]));
                }
                if (provider) words.Insert(words.Count > 0 ? 1 : 0, "Mission Provider");

                var name = string.Join(" ", words.ToArray());
                if (suffixes.Count > 0) name += " (" + string.Join(", ", suffixes.ToArray()) + ")";
                return name.Length == 0 ? shortName : name;
            }

            // One prefab word. A lone letter is a variant label (`_a`, `_b`); a trailing
            // copy number is dropped, because `wolf2` is the same animal as `wolf`.
            private static string Word(string part)
            {
                string glued;
                if (Glued.TryGetValue(part, out glued)) return glued;
                if (part.Length == 1) return part.ToUpperInvariant();

                var end = part.Length;
                while (end > 0 && char.IsDigit(part[end - 1])) end--;
                if (end > 0 && end < part.Length && char.IsLetter(part[end - 1]))
                {
                    var stem = part.Substring(0, end);
                    if (Glued.TryGetValue(stem, out glued)) return glued;
                    part = stem;
                }
                return char.ToUpperInvariant(part[0]) + part.Substring(1);
            }
        }
        // takaro:names-end

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
            // Rust's console prints nothing for most commands it is handed (`say` among
            // them), so this line is the only record an operator has of what Takaro ran on
            // their server -- and the only thing outside the connector that shows a console
            // round trip happened at all. Only the verb goes in it: the arguments are
            // whatever Takaro was asked to run, up to and including a password an admin
            // typed, and this log is read by anyone who can read the server console.
            LogInfo($"console: {CommandSummary(command)}");
            try
            {
                var result = ConsoleSystem.Run(ConsoleSystem.Option.Server, command) ?? "";
                // A command that throws does not throw here: Rust hands back its message
                // as "Error: <command> - <message>" in place of the output.
                var failed = result.StartsWith("Error: ", StringComparison.Ordinal);
                return new JObject
                {
                    ["success"] = !failed,
                    ["rawResult"] = result,
                    ["errorMessage"] = failed ? result : ""
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
                // Same reasoning as the console audit line above: a broadcast leaves no
                // trace in Rust's own console, so nothing on the server would ever show
                // that Takaro said something to everybody.
                LogInfo($"broadcast: {message}");
                ConsoleNetwork.BroadcastToAllClients("chat.add", 2, 0, message);
            }
        }

        private void HandleGiveItem(JObject args)
        {
            var gameId = GameIdFrom(args);
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
            var gameId = GameIdFrom(args);
            var x = args.Value<float?>("x") ?? 0;
            var y = args.Value<float?>("y") ?? 0;
            var z = args.Value<float?>("z") ?? 0;

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new Exception("Player not found");

            player.Teleport(new Vector3(x, y, z));
        }

        private void HandleKickPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var reason = args.Value<string>("reason") ?? "";

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new Exception("Player not found");

            player.Kick(reason);
        }

        private void HandleBanPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var reason = args.Value<string>("reason") ?? "";
            var expiresAt = args.Value<string>("expiresAt");

            if (string.IsNullOrEmpty(gameId)) throw new Exception("gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new Exception("Invalid gameId");

            var player = FindPlayerByGameId(gameId);
            var name = player?.displayName ?? KnownPlayerName(steamId) ?? gameId;

            DateTimeOffset expiry;
            if (TryParseExpiry(expiresAt, out expiry))
            {
                // Timed ban: keep it in our own data file, native ban list has no expiry.
                ServerUsers.Remove(steamId);
                ServerUsers.Save();

                _timedBans[steamId] = new TimedBan
                {
                    SteamId = gameId,
                    Name = name,
                    Reason = reason,
                    ExpiresAt = ToIso8601(expiry)
                };
                SaveTimedBans();
                LogInfo($"Timed ban for {gameId} until {ToIso8601(expiry)}");
            }
            else
            {
                if (_timedBans.Remove(steamId)) SaveTimedBans();

                ServerUsers.Set(steamId, ServerUsers.UserGroup.Banned, name, reason);
                ServerUsers.Save();
            }

            player?.Kick($"Banned: {reason}");
        }

        private void HandleUnbanPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            if (string.IsNullOrEmpty(gameId)) throw new Exception("gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new Exception("Invalid gameId");

            if (_timedBans.Remove(steamId)) SaveTimedBans();

            ServerUsers.Remove(steamId);
            ServerUsers.Save();
        }

        // Takaro resolves every listed ban to a player through the same platform-id match as
        // events; a ban entry with only a gameId cannot be matched to a player Takaro has not
        // already seen on this server.
        private static JObject BanPlayerJson(string id, string name)
        {
            if (!IsSteamPlayerId(id)) return new JObject { ["gameId"] = id ?? "", ["name"] = name ?? "" };

            // Takaro copies a ban entry's name onto the matched player, so an offline ban that
            // stored the SteamID as its name renamed that player to the number.
            if (string.IsNullOrEmpty(name) || name == id)
                name = KnownPlayerName(ulong.Parse(id)) ?? name;
            return IdentityJson(id, name);
        }

        // Last name Rust saved for a player who is neither online nor sleeping.
        private static string KnownPlayerName(ulong steamId)
        {
            try
            {
                var name = SingletonComponent<ServerMgr>.Instance?.persistance?.GetPlayerName(steamId);
                return string.IsNullOrEmpty(name) ? null : name;
            }
            catch (Exception)
            {
                return null;
            }
        }

        private JToken HandleListBans()
        {
            PruneExpiredBans();

            var arr = new JArray();
            var bans = ServerUsers.GetAll(ServerUsers.UserGroup.Banned);
            foreach (var ban in bans)
            {
                arr.Add(new JObject
                {
                    ["player"] = BanPlayerJson(ban.steamid.ToString(), ban.username),
                    ["reason"] = ban.notes ?? "",
                    ["expiresAt"] = null
                });
            }

            foreach (var ban in _timedBans.Values)
            {
                arr.Add(new JObject
                {
                    ["player"] = BanPlayerJson(ban.SteamId, ban.Name),
                    ["reason"] = ban.Reason ?? "",
                    ["expiresAt"] = ban.ExpiresAt
                });
            }

            return arr;
        }

        // --- Timed Ban Storage & Enforcement ---

        private static bool TryParseExpiry(string expiresAt, out DateTimeOffset expiry)
        {
            expiry = default(DateTimeOffset);
            if (string.IsNullOrEmpty(expiresAt)) return false;

            DateTimeOffset parsed;
            if (!DateTimeOffset.TryParse(
                    expiresAt,
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AllowWhiteSpaces | DateTimeStyles.AssumeUniversal,
                    out parsed))
            {
                return false;
            }

            expiry = parsed.ToUniversalTime();
            return expiry > DateTimeOffset.UtcNow;
        }

        private static string ToIso8601(DateTimeOffset value) =>
            value.ToUniversalTime().ToString("o", CultureInfo.InvariantCulture);

        private void LoadTimedBans()
        {
            _timedBans.Clear();
            try
            {
                var stored = Interface.Oxide.DataFileSystem
                    .ReadObject<Dictionary<string, TimedBan>>(BanDataFile);
                if (stored == null) return;

                foreach (var entry in stored)
                {
                    ulong steamId;
                    if (entry.Value == null || !ulong.TryParse(entry.Key, out steamId)) continue;
                    if (string.IsNullOrEmpty(entry.Value.SteamId)) entry.Value.SteamId = entry.Key;
                    _timedBans[steamId] = entry.Value;
                }
            }
            catch (Exception ex)
            {
                LogWarning($"Could not read timed ban data: {ex.Message}");
            }

            PruneExpiredBans();
        }

        private void SaveTimedBans()
        {
            try
            {
                var toStore = new Dictionary<string, TimedBan>();
                foreach (var entry in _timedBans) toStore[entry.Key.ToString()] = entry.Value;
                Interface.Oxide.DataFileSystem.WriteObject(BanDataFile, toStore);
            }
            catch (Exception ex)
            {
                LogWarning($"Could not write timed ban data: {ex.Message}");
            }
        }

        /// <summary>Drops timed bans whose expiry has passed. Returns true if anything changed.</summary>
        private bool PruneExpiredBans()
        {
            if (_timedBans.Count == 0) return false;

            var now = DateTimeOffset.UtcNow;
            var expired = new List<ulong>();
            foreach (var entry in _timedBans)
            {
                DateTimeOffset expiry = default(DateTimeOffset);
                var parseable = !string.IsNullOrEmpty(entry.Value?.ExpiresAt)
                    && DateTimeOffset.TryParse(
                        entry.Value.ExpiresAt,
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AllowWhiteSpaces | DateTimeStyles.AssumeUniversal,
                        out expiry);

                // An unreadable expiry would never lift, so drop it rather than ban forever.
                if (!parseable || expiry.ToUniversalTime() <= now) expired.Add(entry.Key);
            }

            if (expired.Count == 0) return false;

            foreach (var steamId in expired)
            {
                _timedBans.Remove(steamId);
                LogInfo($"Timed ban for {steamId} expired, lifted");
            }
            SaveTimedBans();
            return true;
        }

        private object CanUserLogin(string name, string id, string ip)
        {
            ulong steamId;
            if (!ulong.TryParse(id, out steamId)) return null;

            TimedBan ban;
            if (!_timedBans.TryGetValue(steamId, out ban)) return null;

            PruneExpiredBans();
            if (!_timedBans.TryGetValue(steamId, out ban)) return null;

            return string.IsNullOrEmpty(ban.Reason) ? "Banned" : $"Banned: {ban.Reason}";
        }

        // The response is sent after this returns, so quitting here would kill the
        // process before Takaro hears back. Quit a moment later instead.
        private void HandleShutdown()
        {
            timer.Once(2f, () => ConsoleSystem.Run(ConsoleSystem.Option.Server, "quit"));
        }

        // --- Game Event Hooks ---

        private void OnPlayerConnected(BasePlayer player)
        {
            if (!IsRealPlayer(player)) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-connected", data);
        }

        private void OnPlayerDisconnected(BasePlayer player, string reason)
        {
            if (!IsRealPlayer(player)) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-disconnected", data);
        }

        private object OnPlayerChat(BasePlayer player, string message, ConVar.Chat.ChatChannel channel)
        {
            if (!IsRealPlayer(player)) return null;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player),
                ["channel"] = channel.ToString().ToLower(),
                ["msg"] = message
            };
            SendGameEvent("chat-message", data);
            return null;
        }

        private void OnPlayerDeath(BasePlayer player, object info)
        {
            if (!IsRealPlayer(player)) return;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };

            var attacker = GetHitInfoInitiatorPlayer(info);
            if (IsRealPlayer(attacker) && attacker != player)
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

        private void OnEntityDeath(BaseCombatEntity entity, object info)
        {
            // Real players die through OnPlayerDeath. NPCs (scientists etc.) are BasePlayers too
            // and are listed by listEntities, so a player killing one is an entity kill.
            if (entity == null || (entity is BasePlayer victim && !victim.IsNpc)) return;

            var attacker = GetHitInfoInitiatorPlayer(info);
            if (!IsRealPlayer(attacker)) return;

            var weapon = GetHitInfoWeaponShortName(info);
            var data = new JObject
            {
                ["player"] = PlayerToJson(attacker),
                ["entity"] = entity.ShortPrefabName ?? entity.GetType().Name,
                ["weapon"] = weapon
            };
            SendGameEvent("entity-killed", data);
        }

        private BasePlayer GetHitInfoInitiatorPlayer(object info)
        {
            if (info == null) return null;

            try
            {
                return GetMemberValue(info, "InitiatorPlayer") as BasePlayer;
            }
            catch (Exception ex)
            {
                LogDebug($"Unable to read HitInfo.InitiatorPlayer: {ex.Message}");
                return null;
            }
        }

        private string GetHitInfoWeaponShortName(object info)
        {
            if (info == null) return "";

            try
            {
                var weapon = GetMemberValue(info, "Weapon");
                var item = weapon?.GetType().GetMethod("GetItem", Type.EmptyTypes)?.Invoke(weapon, null);
                var itemInfo = GetMemberValue(item, "info");
                var shortName = GetMemberValue(itemInfo, "shortname") as string;
                if (!string.IsNullOrEmpty(shortName)) return shortName;

                var weaponPrefab = GetMemberValue(info, "WeaponPrefab");
                return GetMemberValue(weaponPrefab, "ShortPrefabName") as string ?? "";
            }
            catch (Exception ex)
            {
                LogDebug($"Unable to read HitInfo weapon: {ex.Message}");
                return "";
            }
        }

        private object GetMemberValue(object instance, string name)
        {
            if (instance == null) return null;

            var type = instance.GetType();
            var property = type.GetProperty(name);
            if (property != null) return property.GetValue(instance, null);

            var field = type.GetField(name);
            return field?.GetValue(instance);
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

        // Carbon's file logger throws when the disk is full; a throw from a log call inside the
        // reconnect path ends the reconnect loop and leaves the server offline for good.
        private void LogInfo(string message) { try { Puts($"[Takaro] {message}"); } catch { } }
        private void LogWarning(string message) { try { PrintWarning($"[Takaro] {message}"); } catch { } }
        private void LogDebug(string message) { if (_debug) try { Puts($"[Takaro DEBUG] {message}"); } catch { } }
    }
}
