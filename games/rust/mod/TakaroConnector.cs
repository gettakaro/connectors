using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
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
    [Info("TakaroConnector", "Takaro", "0.1.4")] // x-release-please-version
    [Description("Takaro Generic Connector — connects outbound to Takaro via WebSocket")]
    public class TakaroConnector : RustPlugin
    {
        // --- Configuration ---

        // takaro:config-begin
        // The plugin's config is <framework config dir>/TakaroConnector.json, created on first
        // load. Environment variables, when set and non-empty, win over the file field by
        // field, so a Docker or systemd install that sets them behaves exactly as before.
        internal const string DefaultWsUrl = "wss://connect.takaro.io/";
        internal const string EnvWsUrl = "TAKARO_WS_URL";
        internal const string EnvRegistrationToken = "TAKARO_REGISTRATION_TOKEN";
        internal const string EnvIdentityToken = "TAKARO_IDENTITY_TOKEN";
        internal const string EnvDebug = "TAKARO_DEBUG";

        internal sealed class ConnectorSettings
        {
            public string WsUrl = DefaultWsUrl;
            public string RegistrationToken = "";
            public string IdentityToken = "";
            public bool Debug;
            public bool RegistrationFromEnv;
            public bool IdentityFromEnv;

            public bool SameConnection(ConnectorSettings other)
            {
                return other != null
                    && WsUrl == other.WsUrl
                    && RegistrationToken == other.RegistrationToken
                    && IdentityToken == other.IdentityToken;
            }
        }

        // What the file says; null means the key is absent.
        internal sealed class ConfigFileValues
        {
            public string RegistrationToken;
            public string IdentityToken;
            public string WsUrl;
            public bool? Debug;

            public bool HasAnyKey
            {
                get { return RegistrationToken != null || IdentityToken != null || WsUrl != null || Debug != null; }
            }
        }

        // Throws on anything that is not a JSON object: the caller keeps its settings.
        internal static ConfigFileValues ParseConfigText(string text)
        {
            var json = JObject.Parse(text);
            var values = new ConfigFileValues
            {
                RegistrationToken = StringField(json, "RegistrationToken"),
                IdentityToken = StringField(json, "IdentityToken"),
                WsUrl = StringField(json, "WebSocketUrl"),
            };
            var debug = json["Debug"];
            if (debug != null && debug.Type == JTokenType.Boolean)
                values.Debug = debug.Value<bool>();
            else if (debug != null && debug.Type == JTokenType.String)
                values.Debug = string.Equals(debug.Value<string>()?.Trim(), "true", StringComparison.OrdinalIgnoreCase);
            return values;
        }

        private static string StringField(JObject json, string key)
        {
            var token = json[key];
            if (token == null) return null;
            if (token.Type == JTokenType.Null) return "";
            return (token.Type == JTokenType.String ? token.Value<string>() : token.ToString()).Trim();
        }

        internal static string RenderConfigText(ConfigFileValues values)
        {
            var json = new JObject
            {
                ["RegistrationToken"] = values.RegistrationToken ?? "",
                ["IdentityToken"] = values.IdentityToken ?? "",
                ["WebSocketUrl"] = string.IsNullOrEmpty(values.WsUrl) ? DefaultWsUrl : values.WsUrl,
                ["Debug"] = values.Debug ?? false,
            };
            return json.ToString(Formatting.Indented) + "\n";
        }

        // While running: a missing, empty or unparseable file is one being saved or edited,
        // so it yields nothing and the current settings stay.
        internal static bool TryParseRunningConfig(string text, out ConfigFileValues values)
        {
            values = null;
            if (string.IsNullOrWhiteSpace(text)) return false;
            try { values = ParseConfigText(text); }
            catch { return false; }
            return true;
        }

        // A file the plugin never wrote: absent, empty, or the "{}" a framework may leave.
        internal static bool IsFreshConfig(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return true;
            try { return !ParseConfigText(text).HasAnyKey; }
            catch { return false; }
        }

        // The file a first load writes. A fresh install gets a new identity; an install that
        // already connects through TAKARO_REGISTRATION_TOKEN or TAKARO_IDENTITY_TOKEN keeps the
        // identity it has been sending (possibly the empty one), so its Takaro server record
        // and players stay where they are.
        internal static ConfigFileValues FirstConfig(Func<string, string> env, Func<string> newIdentity)
        {
            var existingEnvInstall = !string.IsNullOrEmpty(env(EnvRegistrationToken))
                || !string.IsNullOrEmpty(env(EnvIdentityToken));
            return new ConfigFileValues
            {
                RegistrationToken = "",
                IdentityToken = existingEnvInstall ? "" : newIdentity(),
                WsUrl = DefaultWsUrl,
                Debug = false,
            };
        }

        // `current` is what is in use (null on first load): an absent key while running is a
        // file being edited, so it keeps the current token instead of dropping it.
        internal static ConnectorSettings ResolveSettings(ConfigFileValues file, Func<string, string> env, ConnectorSettings current)
        {
            var settings = new ConnectorSettings();

            var envUrl = env(EnvWsUrl);
            settings.WsUrl = !string.IsNullOrEmpty(envUrl) ? envUrl
                : !string.IsNullOrEmpty(file.WsUrl) ? file.WsUrl
                : DefaultWsUrl;

            var envRegistration = env(EnvRegistrationToken);
            settings.RegistrationFromEnv = !string.IsNullOrEmpty(envRegistration);
            settings.RegistrationToken = settings.RegistrationFromEnv ? envRegistration
                : file.RegistrationToken ?? (current != null && !current.RegistrationFromEnv ? current.RegistrationToken : "");

            var envIdentity = env(EnvIdentityToken);
            settings.IdentityFromEnv = !string.IsNullOrEmpty(envIdentity);
            settings.IdentityToken = settings.IdentityFromEnv ? envIdentity
                : file.IdentityToken ?? (current != null && !current.IdentityFromEnv ? current.IdentityToken : "");

            var envDebug = env(EnvDebug);
            settings.Debug = !string.IsNullOrEmpty(envDebug)
                ? envDebug.ToLower() == "true"
                : file.Debug ?? false;
            return settings;
        }
        // takaro:config-end

        private const string ConfigFileName = "TakaroConnector.json";
        private const int ConfigCheckEverySeconds = 5;

        private volatile ConnectorSettings _settings = new ConnectorSettings();
        private string _configPath;
        private string _configText;
        private string _configProblem;
        private Timer _tickTimer;
        private int _tickCount;

        private bool _debug { get { return _settings.Debug; } }

        private const long InitialReconnectDelay = 5000;
        private const long MaxReconnectDelay = 300000;
        private const double BackoffMultiplier = 1.5;

        // --- WebSocket State ---
        //
        // Connections are started on the main thread only (Init, the 1 s tick). Every
        // connection carries the generation it was started under; a socket task, close or
        // reconnect request from an older generation is stale and changes nothing.

        private ClientWebSocket _ws;
        private CancellationTokenSource _cts;
        private volatile bool _shouldReconnect = true;
        private long _currentReconnectDelay = InitialReconnectDelay;
        private volatile bool _connected;
        private int _generation;
        private int _identifySentGeneration = -1;
        // An auth close stops retries for that connection only; a config change starts a new one.
        private int _authClosedGeneration = -1;
        private ReconnectRequest _pendingReconnect;
        private readonly object _sendLock = new object();

        private sealed class ReconnectRequest
        {
            public int Generation;
            public long DueTicks;
        }
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

            _configPath = Path.GetFullPath(Path.Combine(Interface.Oxide.ConfigDirectory, ConfigFileName));
            LoadSettings();
            _tickTimer = timer.Every(1f, Tick);

            var settings = _settings;
            LogInfo($"Config: {_configPath} (registration token {TokenSource(settings)})");
            if (settings.RegistrationFromEnv)
                LogInfo($"{EnvRegistrationToken} is set in the server's environment and overrides RegistrationToken in the config file");

            if (string.IsNullOrEmpty(settings.RegistrationToken))
            {
                LogMissingToken();
                return;
            }

            LogInfo($"Connecting to {settings.WsUrl}");
            StartConnection();
        }

        private void Unload()
        {
            _shouldReconnect = false;
            _connected = false;
            Interlocked.Increment(ref _generation);
            Interlocked.Exchange(ref _pendingReconnect, null);
            _tickTimer?.Destroy();
            _tickTimer = null;
            _cts?.Cancel();
            try { _ws?.Dispose(); } catch { }
            _banSweepTimer?.Destroy();
            _banSweepTimer = null;
            _lastPosition.Clear();
        }

        // --- Config file ---

        private static string EnvValue(string name)
        {
            return Environment.GetEnvironmentVariable(name);
        }

        private static string TokenSource(ConnectorSettings settings)
        {
            if (settings.RegistrationFromEnv) return $"from {EnvRegistrationToken}";
            return string.IsNullOrEmpty(settings.RegistrationToken) ? "not set" : "set";
        }

        private static string ReadConfigText(string path)
        {
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }

        // First load: an absent (or never-written) file is created with an empty token, and a
        // new identity unless the environment already gives this install one.
        private void LoadSettings()
        {
            string text;
            try { text = ReadConfigText(_configPath); }
            catch (Exception ex)
            {
                // Never recreate a file that exists but could not be read: that would replace
                // its token and identity. The 5 s check picks it up once it reads.
                LogWarning($"Could not read {_configPath} ({ex.Message}); trying again in a few seconds");
                _configText = null;
                _configProblem = "";
                _settings = ResolveSettings(new ConfigFileValues(), EnvValue, null);
                return;
            }

            ConfigFileValues file = null;
            if (text == null || IsFreshConfig(text))
            {
                file = FirstConfig(EnvValue, () => Guid.NewGuid().ToString());
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(_configPath));
                    File.WriteAllText(_configPath, RenderConfigText(file));
                    text = File.ReadAllText(_configPath);
                    if (!string.IsNullOrEmpty(file.IdentityToken))
                        LogInfo($"Created {_configPath} with a new identity token");
                    else
                        LogInfo($"Created {_configPath}");
                }
                catch (Exception ex)
                {
                    LogWarning($"Could not create {_configPath}: {ex.Message}");
                }
            }
            else
            {
                try { file = ParseConfigText(text); }
                catch (Exception ex)
                {
                    _configProblem = text;
                    LogWarning($"Could not read {_configPath} ({ex.Message}); fix the file and save it, the plugin re-reads it every few seconds");
                    file = new ConfigFileValues();
                }
            }

            _configText = text;
            _settings = ResolveSettings(file, EnvValue, null);
        }

        // Runs on the main thread every second: re-reads the config every few seconds and
        // starts any reconnect a closed socket asked for.
        private void Tick()
        {
            if (++_tickCount % ConfigCheckEverySeconds == 0)
            {
                try { CheckConfig(); }
                catch (Exception ex) { LogWarning($"Error checking {_configPath}: {ex.Message}"); }
            }

            var request = Interlocked.Exchange(ref _pendingReconnect, null);
            if (request == null || request.Generation != _generation) return;
            if (DateTime.UtcNow.Ticks < request.DueTicks)
            {
                Interlocked.CompareExchange(ref _pendingReconnect, request, null);
                return;
            }
            if (_shouldReconnect && !string.IsNullOrEmpty(_settings.RegistrationToken))
                StartConnection();
        }

        // Compares the whole text: a timestamp can miss a same-length edit. A file that is
        // missing, empty or unparseable mid-save keeps the current settings until next time.
        private void CheckConfig()
        {
            string text;
            try { text = ReadConfigText(_configPath); }
            catch { return; }
            if (text == _configText) { _configProblem = null; return; }

            ConfigFileValues file;
            if (!TryParseRunningConfig(text, out file))
            {
                if (_configProblem != (text ?? ""))
                    LogWarning(string.IsNullOrWhiteSpace(text)
                        ? $"{_configPath} is missing or empty; keeping the current settings. Reload the plugin to recreate it"
                        : $"Could not read {_configPath}; keeping the current settings and trying again in a few seconds");
                _configProblem = text ?? "";
                return;
            }

            _configProblem = null;
            _configText = text;
            var previous = _settings;
            var next = ResolveSettings(file, EnvValue, previous);
            _settings = next;
            if (next.SameConnection(previous)) return;

            LogInfo($"{ConfigFileName} changed; reconnecting with the new settings (registration token {TokenSource(next)})");
            Reconnect();
        }

        // Drops the current socket and connects afresh, skipping whatever backoff a rejected
        // token had built up.
        private void Reconnect()
        {
            Interlocked.Increment(ref _generation);
            Interlocked.Exchange(ref _pendingReconnect, null);
            _connected = false;
            _cts?.Cancel();
            try { _ws?.Dispose(); } catch { }
            _ws = null;
            _currentReconnectDelay = InitialReconnectDelay;
            _shouldReconnect = true;

            if (string.IsNullOrEmpty(_settings.RegistrationToken))
            {
                LogMissingToken();
                return;
            }
            LogInfo($"Connecting to {_settings.WsUrl}");
            StartConnection();
        }

        // Config problems stop the connection outright, so they must stand out in a busy
        // server console.
        private void LogBanner(params string[] lines)
        {
            const string rule = "*************************************************************************";
            var text = new StringBuilder();
            text.Append(rule);
            foreach (var line in lines)
                text.Append("\n  ").Append(line);
            text.Append('\n').Append(rule);
            try { PrintWarning("\n" + text); } catch { }
        }

        private void LogMissingToken()
        {
            LogBanner(
                "RegistrationToken not set, the server is not connected to Takaro.",
                $"Paste the registration token from Takaro into {_configPath}",
                "and save it. The plugin connects within a few seconds, no restart needed.");
        }

        private void LogRejected(string reason)
        {
            if (_settings.RegistrationFromEnv)
            {
                LogBanner(
                    $"Takaro rejected identify: {reason}.",
                    $"Check {EnvRegistrationToken} in the server's environment; it overrides",
                    $"{_configPath}. The environment is only read when the server starts.");
                return;
            }
            LogBanner(
                $"Takaro rejected identify: {reason}.",
                $"Check RegistrationToken in {_configPath}. Saving a corrected",
                "token reconnects within a few seconds, no restart needed.");
        }

        // --- WebSocket Connection ---

        // Main thread only.
        private void StartConnection()
        {
            var generation = Interlocked.Increment(ref _generation);
            var settings = _settings;
            _cts?.Cancel();
            var cts = new CancellationTokenSource();
            _cts = cts;
            var token = cts.Token;
            var ws = new ClientWebSocket();
            _ws = ws;

            Task.Run(async () =>
            {
                try
                {
                    await ws.ConnectAsync(new Uri(settings.WsUrl), token);
                    if (!IsCurrent(generation)) return;
                    LogInfo("WebSocket connected");
                    // Identify is the first frame this connector sends, the way every other
                    // Takaro connector in this repository does it. Waiting to be greeted
                    // instead means a peer that expects the client to speak first never
                    // hears from this server at all: the socket sits open and silent.
                    SendIdentify(ws, generation, settings);
                    await ReceiveLoop(ws, generation, settings, token);
                }
                catch (OperationCanceledException) { }
                catch (Exception ex)
                {
                    if (IsCurrent(generation))
                        LogWarning($"WebSocket connection failed: {ex.Message}");
                }
                finally
                {
                    try { ws.Dispose(); } catch { }
                    if (IsCurrent(generation))
                    {
                        _connected = false;
                        Interlocked.CompareExchange(ref _ws, null, ws);
                        if (_shouldReconnect && Volatile.Read(ref _authClosedGeneration) != generation)
                            ScheduleReconnect(generation);
                    }
                }
            });
        }

        private bool IsCurrent(int generation)
        {
            return Volatile.Read(ref _generation) == generation;
        }

        private async Task ReceiveLoop(ClientWebSocket ws, int generation, ConnectorSettings settings, CancellationToken token)
        {
            var buffer = new byte[8192];

            while (ws.State == WebSocketState.Open && !token.IsCancellationRequested)
            {
                var sb = new StringBuilder();
                WebSocketReceiveResult result;

                do
                {
                    result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), token);
                    sb.Append(Encoding.UTF8.GetString(buffer, 0, result.Count));
                } while (!result.EndOfMessage);

                if (!IsCurrent(generation)) break;

                if (result.MessageType == WebSocketMessageType.Close)
                {
                    var code = (int)(result.CloseStatus ?? WebSocketCloseStatus.NormalClosure);
                    var reason = result.CloseStatusDescription ?? "";
                    LogInfo($"WebSocket closed (code={code}, reason={reason})");

                    if (code == 1008 || code == 4001 || code == 4003)
                    {
                        Volatile.Write(ref _authClosedGeneration, generation);
                        LogRejected(string.IsNullOrEmpty(reason) ? $"connection closed with code {code}" : reason);
                    }
                    break;
                }

                var message = sb.ToString();
                if (_debug)
                    LogDebug($"WS RECV: {FrameSummary(message)}");

                try
                {
                    OnWsMessage(message, ws, generation, settings);
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

        private void OnWsMessage(string message, ClientWebSocket ws, int generation, ConnectorSettings settings)
        {
            var json = JObject.Parse(message);
            var type = json.Value<string>("type") ?? "";

            switch (type)
            {
                case "connected":
                    // Takaro greets a fresh connection. Identify has already gone out when
                    // the socket opened, so this only does anything if the greeting beat it.
                    LogInfo("Received server hello");
                    SendIdentify(ws, generation, settings);
                    break;

                case "identifyResponse":
                    HandleIdentifyResponse(json);
                    break;

                case "request":
                    HandleRequest(json, ws, generation);
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

        // Called from a socket task; the main-thread tick starts the connection when it is due.
        private void ScheduleReconnect(int generation)
        {
            if (!_shouldReconnect) return;

            var delaySec = _currentReconnectDelay / 1000.0f;
            LogInfo($"Reconnecting in {delaySec:F0}s...");

            Interlocked.Exchange(ref _pendingReconnect, new ReconnectRequest
            {
                Generation = generation,
                DueTicks = DateTime.UtcNow.Ticks + _currentReconnectDelay * TimeSpan.TicksPerMillisecond,
            });

            _currentReconnectDelay = Math.Min(
                (long)(_currentReconnectDelay * BackoffMultiplier),
                MaxReconnectDelay
            );
        }

        // --- Send Helpers ---

        private void WsSend(string message)
        {
            WsSendOn(_ws, message);
        }

        // Identify and responses go out on the socket they belong to, never on one a config
        // reconnect has put in its place.
        private void WsSendOn(ClientWebSocket ws, string message)
        {
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

        private void SendIdentify(ClientWebSocket ws, int generation, ConnectorSettings settings)
        {
            // At most once per connection: the socket opening and a server greeting both
            // lead here, and a second identify would look like a second session.
            if (Interlocked.Exchange(ref _identifySentGeneration, generation) == generation) return;

            var msg = new JObject
            {
                ["type"] = "identify",
                ["payload"] = new JObject
                {
                    ["identityToken"] = settings.IdentityToken ?? "",
                    ["registrationToken"] = settings.RegistrationToken ?? ""
                }
            };
            LogDebug("WS SEND identify (tokens redacted)");
            WsSendOn(ws, msg.ToString(Formatting.None));
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
                LogRejected(errorMessage);
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

        private void SendResponse(ClientWebSocket ws, string requestId, JToken payload, string error)
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

            WsSendOn(ws, msg.ToString(Formatting.None));
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

        private void HandleRequest(JObject json, ClientWebSocket ws, int generation)
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
                SendResponse(ws, requestId, null, $"Invalid args JSON: {ex.Message}");
                return;
            }

            switch (action)
            {
                case "testReachability":
                    SendResponse(ws, requestId, new JObject { ["connectable"] = true, ["reason"] = null }, null);
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
                    RunOnMainThread(ws, generation, requestId, action, args);
                    break;

                default:
                    SendResponse(ws, requestId, null, $"Action not implemented: {action}");
                    break;
            }
        }

        private void RunOnMainThread(ClientWebSocket ws, int generation, string requestId, string action, JObject args)
        {
            NextTick(() =>
            {
                // A request from a connection that has since been replaced is dropped: its
                // socket is gone, and Takaro re-sends what it still wants on the new one.
                if (!IsCurrent(generation))
                {
                    LogInfo($"Dropped {action} from a replaced connection (requestId={requestId})");
                    return;
                }
                try
                {
                    var result = ExecuteAction(action, args);
                    SendResponse(ws, requestId, result, null);
                }
                catch (Exception ex)
                {
                    LogWarning($"Action {action} failed: {ex.Message}");
                    SendResponse(ws, requestId, null, ex.Message);
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
