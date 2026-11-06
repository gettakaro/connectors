using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Eleon;
using Eleon.Modding;
using MsgChannel = Eleon.MsgChannel;
using SenderType = Eleon.SenderType;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Core;
using TakaroEmpyrion.Map;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Game
{
    /// <summary>The v2 (IModApi) surface the connector needs, wrapped so tests can fake it.</summary>
    public interface IGameV2
    {
        void SendChatMessage(MessageData msg);
        Dictionary<string, int> GetBlockAndItemMapping();
        IEnumerable<string> ContentDirs();
        string SaveGameDir();
    }

    /// <summary>A structured failure answered as {success:false} / error frame, never a fake success.</summary>
    public sealed class ActionFailure : Exception
    {
        public readonly string Code;
        public ActionFailure(string message, string code = ErrorCodes.GameError) : base(message) { Code = code; }
    }

    /// <summary>Implements each Takaro action against the Empyrion server. No WebSocket types here.</summary>
    public sealed class Actions
    {
        readonly GameBridge _game;
        readonly IGameV2 _v2;
        readonly PlayerRegistry _players;
        readonly Catalog _catalog;
        readonly TelnetConsole _telnet;
        readonly Config _cfg;
        readonly ILog _log;
        readonly BanReasons _banReasons;
        readonly Action<string> _onShutdown;
        readonly MapService _map;
        readonly object _catalogLock = new object();
        Task _catalogTask;

        public Actions(GameBridge game, IGameV2 v2, PlayerRegistry players, Catalog catalog, TelnetConsole telnet, Config cfg,
            ILog log, BanReasons banReasons, Action<string> onShutdown, MapService map = null)
        {
            _game = game; _v2 = v2; _players = players; _catalog = catalog; _telnet = telnet; _cfg = cfg; _log = log;
            _banReasons = banReasons; _onShutdown = onShutdown; _map = map;
        }

        public static readonly string[] Supported =
        {
            "testReachability", "getPlayers", "getPlayer", "getPlayerLocation", "getPlayerInventory", "getPlayerProgression", "giveItem", "listItems",
            "listEntities", "listLocations", "executeConsoleCommand", "sendMessage", "teleportPlayer", "kickPlayer", "banPlayer",
            "unbanPlayer", "listBans", "shutdown", "getMapTile",
        };

        /// <summary>
        /// What a v1 identify declares: everything handled except listLocations, which has no honest answer
        /// (always empty), so Takaro reports it unsupported instead of an empty list.
        /// </summary>
        public static IEnumerable<string> Declared { get { return Supported.Where(a => a != "listLocations"); } }

        /// <summary>The v1 error code and the message for a failed request.</summary>
        public static void Classify(Exception e, out string code, out string message)
        {
            Exception inner = e is AggregateException ae && ae.InnerException != null ? ae.InnerException : e;
            message = inner.Message;
            if (inner is UnknownActionException) code = ErrorCodes.Unsupported;
            else if (inner is ArgsException) code = ErrorCodes.InvalidArgs;
            else if (inner is MapNotFoundException) code = ErrorCodes.NotFound;
            else if (inner is ActionFailure af) code = af.Code;
            else if (inner is GameRequestException) code = ErrorCodes.GameError;
            else { code = ErrorCodes.Internal; message = inner.GetType().Name + ": " + inner.Message; }
        }

        public async Task<JToken> Run(string action, Args a)
        {
            switch (action)
            {
                case "testReachability": return await TestReachability();
                case "getPlayers": return await GetPlayers();
                case "getPlayer": return await GetPlayer(a.RequirePlayerGameId());
                case "getPlayerLocation": return await GetPlayerLocation(a.RequirePlayerGameId());
                case "getPlayerInventory": return await GetPlayerInventory(a.RequirePlayerGameId());
                case "getPlayerProgression": return GetPlayerProgression(a.RequirePlayerGameId());
                case "giveItem": return await GiveItem(a);
                case "listItems": await EnsureCatalogFresh(); return _catalog.ItemsJson();
                case "listEntities": await EnsureCatalogFresh(); return _catalog.EntitiesJson();
                case "listLocations": return new JArray();
                case "executeConsoleCommand": return await ExecuteConsoleCommand(a.Str("command"));
                case "sendMessage": return await SendMessage(a);
                case "teleportPlayer": return await Teleport(a);
                case "kickPlayer": return await Kick(a);
                case "banPlayer": return await Ban(a);
                case "unbanPlayer": return await Unban(a.RequirePlayerGameId());
                case "listBans": return await ListBans();
                case "shutdown": return await Shutdown();
                case "getMapTile": return await Task.Run(() => GetMapTile(a));
                default: throw new UnknownActionException(action);
            }
        }

        // ---------------------------------------------------------------- map

        /// <summary>Protocol v1 getMapTile {dimension, zoom, x, y} -> {mimeType, data}; rendered off the game thread.</summary>
        JToken GetMapTile(Args a)
        {
            if (_map == null) throw new UnknownActionException("getMapTile");
            string dim = a.Str("dimension");
            double? zoom = a.Num("zoom"), x = a.Num("x"), y = a.Num("y");
            if (string.IsNullOrEmpty(dim) || zoom == null || x == null || y == null)
                throw new ArgsException("getMapTile takes { dimension, zoom, x, y }");
            byte[] png;
            try { png = _map.Tile(dim, (int)zoom.Value, (int)x.Value, (int)y.Value); }
            catch (ArgumentOutOfRangeException e) { throw new ArgsException(e.Message.Split('\n')[0]); }
            if (png == null) throw new MapNotFoundException("tile " + zoom + "/" + x + "/" + y + " lies outside planet " + dim);
            return new JObject { ["mimeType"] = "image/png", ["data"] = Convert.ToBase64String(png) };
        }

        /// <summary>The playfield behind a Dimension id (ids replace spaces and other characters Takaro refuses).</summary>
        string PlayfieldOf(string dimension, PlayerRecord current)
        {
            if (current != null && Planet.DimensionId(current.Playfield) == dimension) return current.Playfield;
            Planet p = _map?.Find(dimension);
            if (p != null) return p.Name;
            PlayerRecord other = _players.All().FirstOrDefault(r => r.Playfield != null && Planet.DimensionId(r.Playfield) == dimension);
            return other?.Playfield ?? dimension;
        }

        // ---------------------------------------------------------------- server

        async Task<JToken> TestReachability()
        {
            if (!_game.Attached)
                return new JObject { ["connectable"] = false, ["reason"] = "the Empyrion mod API has not started yet" };
            try
            {
                await _game.Request(CmdId.Request_Dedi_Stats, null);
                return new JObject { ["connectable"] = true, ["reason"] = null };
            }
            catch (Exception e)
            {
                return new JObject { ["connectable"] = false, ["reason"] = "dedicated server did not answer: " + e.Message };
            }
        }

        async Task<JToken> Shutdown()
        {
            if (!_cfg.EnableDestructiveCommands)
                throw new ActionFailure("shutdown is disabled; set ENABLE_DESTRUCTIVE_COMMANDS=true in TakaroConfig.txt to allow it");
            _onShutdown("saveandexit 0");
            return JValue.CreateNull();
        }

        /// <summary>
        /// saveandexit is refused while the dedi is starting a playfield server ("Cannot exit now ... retry in 30
        /// seconds"), which happens right after a player joins. The reply to Takaro has already gone out, so the
        /// connector keeps retrying until the game accepts or the attempts run out, and logs every refusal.
        /// </summary>
        public static async Task<string> RunUntilAccepted(Func<Task<string>> run, Func<TimeSpan, Task> delay, ILog log,
            int maxAttempts = 12, int retrySeconds = 15)
        {
            string reply = null;
            for (int attempt = 1; attempt <= maxAttempts; attempt++)
            {
                reply = await run() ?? "";
                if (!IsRetryRefusal(reply)) return reply;
                log.Warn("shutdown refused by the game (attempt " + attempt + "/" + maxAttempts + "): " + LogText.Clip(reply, 200));
                if (attempt < maxAttempts) await delay(TimeSpan.FromSeconds(retrySeconds));
            }
            log.Error("shutdown gave up after " + maxAttempts + " refusals");
            return reply;
        }

        public static bool IsRetryRefusal(string reply)
        {
            return reply != null && (reply.IndexOf("Cannot exit now", StringComparison.OrdinalIgnoreCase) >= 0
                                     || reply.IndexOf("retry in", StringComparison.OrdinalIgnoreCase) >= 0);
        }

        // ---------------------------------------------------------------- players

        public async Task<PlayerRecord> RefreshPlayer(int entityId)
        {
            var info = (PlayerInfo)await _game.Request(CmdId.Request_Player_Info, new Id(entityId));
            PlayerRecord r = _players.Upsert(info.steamId, info.entityId, info.playerName);
            if (r == null) throw new ActionFailure("player " + entityId + " has no SteamID");
            r.Ping = info.ping;
            r.Experience = info.exp;
            r.HasExperience = true;
            _players.SetPosition(r, info.playfield, info.pos.x, info.pos.y, info.pos.z);
            return r;
        }

        public async Task<List<PlayerRecord>> OnlinePlayers()
        {
            var ids = (IdList)await _game.Request(CmdId.Request_Player_List, null);
            var list = new List<PlayerRecord>();
            var seen = new HashSet<string>();
            foreach (int id in ids?.list ?? new List<int>())
            {
                try
                {
                    PlayerRecord r = await RefreshPlayer(id);
                    if (!r.Online) _players.SetOnline(r, true);
                    seen.Add(r.SteamId);
                    list.Add(r);
                }
                catch (Exception e) { _log.Warn("player " + id + " skipped: " + e.Message); }
            }
            if (list.Count > 0) await FillIps(list);
            foreach (PlayerRecord stale in _players.Online().Where(p => !seen.Contains(p.SteamId)))
                _players.SetOnline(stale, false);
            _players.SaveSoon();
            return list;
        }

        /// <summary>Test seam: replaces the telnet "plys" read when set.</summary>
        public Func<string> PlysReader;

        bool CanReadPlys { get { return PlysReader != null || (_telnet != null && _telnet.Configured); } }

        string ReadPlys()
        {
            return PlysReader != null ? PlysReader() : _telnet.Run("plys", TimeSpan.FromMilliseconds(300), TimeSpan.FromSeconds(3));
        }

        /// <summary>The mod API has no client IP; the console's plys lists "C-Id: E-Id, Name, Playfield, ip|port".</summary>
        async Task FillIps(List<PlayerRecord> players)
        {
            if (!CanReadPlys) return;
            try
            {
                string plys = await Task.Run(() => ReadPlys());
                Dictionary<int, string> ips = ParsePlysIps(plys);
                foreach (PlayerRecord r in players) if (ips.TryGetValue(r.EntityId, out string ip)) r.Ip = ip;
            }
            catch (Exception e) { _log.Debug("plys for ip failed: " + e.Message); }
        }

        /// <summary>
        /// Fresh identity for a player-connected event: drops the IP remembered from an earlier session, reads the
        /// current one from plys (the console can list a joining player a moment late, so it retries), and re-reads
        /// the ping when the game still reported 0 at login.
        /// </summary>
        public async Task<PlayerRecord> RefreshConnectingPlayer(int entityId, int attempts = 3, int gapMs = 1000)
        {
            PlayerRecord r = await FirstReading(entityId);
            r.Ip = null;
            for (int i = 0; i < attempts && r.Ip == null && CanReadPlys; i++)
            {
                if (i > 0) await Task.Delay(gapMs);
                await FillIps(new List<PlayerRecord> { r });
            }
            if (r.Ping <= 0)
            {
                try { r = await RefreshPlayer(entityId); } catch (Exception) { }
            }
            return r;
        }

        /// <summary>Test seam: pause between attempts to read a player who is still joining.</summary>
        public int JoinRetryGapMs = 5000;

        /// <summary>
        /// The first player read of a join. The joining client answers it and can stay silent past the request
        /// timeout while it loads, which would lose the player-connected event, so silence is retried. A game error
        /// means the player is gone and fails at once: a retry must not announce someone who already left.
        /// </summary>
        async Task<PlayerRecord> FirstReading(int entityId, int attempts = 4)
        {
            for (int i = 1; ; i++)
            {
                try { return await RefreshPlayer(entityId); }
                catch (GameRequestException e) when (i < attempts && e.ErrorType == ErrorType.None)
                {
                    _log.Warn("player " + entityId + " not readable yet (" + e.Message + "), retrying");
                    await Task.Delay(JoinRetryGapMs);
                }
            }
        }

        public static Dictionary<int, string> ParsePlysIps(string plys)
        {
            var map = new Dictionary<int, string>();
            foreach (Match m in Regex.Matches(plys ?? "", @"^\s*\d+:\s*(\d+),.*,\s*([0-9a-fA-F:.]+)\|\d+\s*$", RegexOptions.Multiline))
                map[int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture)] = m.Groups[2].Value;
            return map;
        }

        /// <summary>
        /// Answers from the last reading and never touches the game thread: Takaro asks on every player sync, right
        /// after getPlayers and getPlayerLocation have refreshed the player. null for a player the server never saw.
        /// </summary>
        JToken GetPlayerProgression(string gameId)
        {
            PlayerRecord r = _players.Resolve(gameId);
            return r == null ? JValue.CreateNull() : (JToken)Progression.Tracks(r);
        }

        async Task<JToken> GetPlayers()
        {
            var arr = new JArray();
            foreach (PlayerRecord r in await OnlinePlayers()) arr.Add(PlayerRegistry.ToTakaro(r));
            return arr;
        }

        async Task<PlayerRecord> OnlineByGameId(string gameId)
        {
            PlayerRecord r = _players.Resolve(gameId);
            if (r != null && r.Online && r.EntityId > 0)
            {
                try { return await RefreshPlayer(r.EntityId); }
                catch (GameRequestException) { /* went offline; fall through to a fresh list */ }
            }
            foreach (PlayerRecord p in await OnlinePlayers())
                if (p.SteamId == gameId || (r != null && p.SteamId == r.SteamId)) return p;
            return null;
        }

        async Task<JToken> GetPlayer(string gameId)
        {
            PlayerRecord r = await OnlineByGameId(gameId) ?? _players.Resolve(gameId);
            if (r == null) return JValue.CreateNull();
            return PlayerRegistry.ToTakaro(r);
        }

        async Task<JToken> GetPlayerLocation(string gameId)
        {
            PlayerRecord r = await OnlineByGameId(gameId);
            if (r == null) return JValue.CreateNull();
            return PlayerRegistry.PositionOf(r);
        }

        async Task<JToken> GetPlayerInventory(string gameId)
        {
            PlayerRecord r = await OnlineByGameId(gameId) ?? throw new ActionFailure("player " + gameId + " is not online");
            await EnsureCatalogFresh();
            var inv = (Inventory)await _game.Request(CmdId.Request_Player_GetInventory, new Id(r.EntityId));
            var arr = new JArray();
            foreach (ItemStack s in (inv.toolbelt ?? new ItemStack[0]).Concat(inv.bag ?? new ItemStack[0]))
            {
                if (s.id <= 0 || s.count <= 0) continue;
                string code = _catalog.CodeOf(s.id);
                arr.Add(new JObject { ["code"] = code, ["name"] = _catalog.DisplayName(code), ["amount"] = s.count });
            }
            return arr;
        }

        // ---------------------------------------------------------------- items

        public Task EnsureCatalog()
        {
            lock (_catalogLock)
            {
                if (_catalogTask == null || _catalogTask.IsFaulted)
                    _catalogTask = LoadCatalog();
                return _catalogTask;
            }
        }

        bool _catalogFromEcf;

        public Task EnsureCatalogFresh()
        {
            Task t = EnsureCatalog();
            if (!_catalogFromEcf || !t.IsCompleted) return t;
            return t.ContinueWith(async _ => await TryRuntimeMapping()).Unwrap();
        }

        async Task LoadCatalog()
        {
            List<string> dirs = _v2.ContentDirs().ToList();
            Dictionary<string, int> mapping = await _game.OnGameThread(() => _v2.GetBlockAndItemMapping());
            if (mapping != null && mapping.Count > 0)
            {
                _catalog.LoadMapping(mapping);
                _catalogFromEcf = false;
            }
            else
            {
                int n = _catalog.LoadMappingFromEcf(dirs);
                if (n == 0) throw new ActionFailure("no item mapping: the game returned none and no ItemsConfig/BlocksConfig was found");
                _catalogFromEcf = true;
                _log.Info("game item mapping not ready yet; using " + n + " ids from ItemsConfig/BlocksConfig");
            }
            _catalog.LoadFiles(dirs);
            _log.Info("catalogue loaded: " + _catalog.Count + " items/blocks, " + _catalog.EntityCount + " creatures" +
                      (_catalogFromEcf ? " (config-derived ids)" : ""));
        }

        /// <summary>Swaps in the game's own mapping once it exists and reports any id the fallback got wrong.</summary>
        async Task TryRuntimeMapping()
        {
            Dictionary<string, int> mapping = await _game.OnGameThread(() => _v2.GetBlockAndItemMapping());
            if (mapping == null || mapping.Count == 0) return;
            List<string> bad = _catalog.Mismatches(mapping);
            if (bad.Count > 0) _log.Warn("config-derived item ids differed for " + bad.Count + " entries, e.g. " + string.Join(", ", bad.Take(5)));
            else _log.Info("game item mapping available (" + mapping.Count + "); config-derived ids all matched");
            _catalog.LoadMapping(mapping);
            _catalogFromEcf = false;
        }

        async Task<JToken> GiveItem(Args a)
        {
            string gameId = a.RequirePlayerGameId();
            string item = a.Str("item") ?? a.Str("code") ?? a.Str("name");
            if (string.IsNullOrEmpty(item)) throw new ArgsException("missing item");
            int amount = (int)Math.Round(a.Num("amount") ?? 1);
            if (amount < 1) throw new ActionFailure("amount must be at least 1");
            await EnsureCatalogFresh();
            if (!_catalog.TryId(item, out int id, out string code)) throw new ActionFailure("unknown item '" + item + "'");
            PlayerRecord r = await OnlineByGameId(gameId) ?? throw new ActionFailure("player " + gameId + " is not online");
            await _game.Request(CmdId.Request_Player_AddItem, new IdItemStack(r.EntityId, new ItemStack(id, amount)));
            _log.Info("gave " + amount + "x " + code + " to " + r.SteamId);
            return JValue.CreateNull();
        }

        // ---------------------------------------------------------------- chat + console

        async Task<JToken> SendMessage(Args a)
        {
            string text = a.Str("message");
            if (string.IsNullOrEmpty(text)) throw new ArgsException("missing message");
            JObject opts = a.Obj("opts");
            string recipient = null, sender = null;
            if (opts != null)
            {
                var o = Args.Parse(opts);
                recipient = o.PlayerGameId() ?? (opts["recipient"] is JObject rc ? Args.Parse(rc).Str("gameId") : null);
                sender = o.Str("senderNameOverride");
            }
            if (string.IsNullOrEmpty(sender)) sender = string.IsNullOrEmpty(_cfg.ChatSenderName) ? _cfg.ServerName : _cfg.ChatSenderName;
            var msg = new MessageData
            {
                SenderType = SenderType.ServerPrio,
                Channel = MsgChannel.Global,
                SenderNameOverride = sender,
                Text = text,
            };
            if (!string.IsNullOrEmpty(recipient))
            {
                PlayerRecord r = await OnlineByGameId(recipient) ?? throw new ActionFailure("recipient " + recipient + " is not online");
                msg.Channel = MsgChannel.SinglePlayer;
                msg.RecipientEntityId = r.EntityId;
            }
            ServerEcho.Remember(text);
            await _game.OnGameThread(() => { _v2.SendChatMessage(msg); return true; });
            return JValue.CreateNull();
        }

        async Task<JToken> ExecuteConsoleCommand(string command)
        {
            if (string.IsNullOrWhiteSpace(command))
                return new JObject { ["success"] = false, ["rawResult"] = "", ["errorMessage"] = "missing command" };
            string verb = command.Trim().Split(' ')[0];
            if (!_cfg.ConsoleAllowlist.Contains(verb))
                return new JObject
                {
                    ["success"] = false, ["rawResult"] = "",
                    ["errorMessage"] = "command_not_allowed: '" + verb + "' is not in CONSOLE_ALLOWLIST",
                };
            try
            {
                string output = await RunConsole(command);
                bool unknown = output.IndexOf("Unknown command", StringComparison.OrdinalIgnoreCase) >= 0;
                return new JObject { ["success"] = !unknown, ["rawResult"] = output, ["errorMessage"] = unknown ? output : null };
            }
            catch (Exception e)
            {
                return new JObject { ["success"] = false, ["rawResult"] = "", ["errorMessage"] = e.Message };
            }
        }

        /// <summary>Telnet when the dedi has it enabled (real output), else the mod API (no output).</summary>
        public async Task<string> RunConsole(string command)
        {
            if (_telnet != null && _telnet.Configured)
            {
                try { return await Task.Run(() => _telnet.Run(command, TimeSpan.FromMilliseconds(400), TimeSpan.FromSeconds(5))); }
                catch (Exception e) { _log.Warn("telnet console failed, using the mod API: " + e.Message); }
            }
            await _game.Request(CmdId.Request_ConsoleCommand, new PString(command));
            return "";
        }

        // ---------------------------------------------------------------- movement

        async Task<JToken> Teleport(Args a)
        {
            string gameId = a.RequirePlayerGameId();
            double? x = a.Num("x"), y = a.Num("y"), z = a.Num("z");
            if (x == null || y == null || z == null) throw new ArgsException("teleportPlayer needs numeric x, y and z");
            string dim = a.Str("dimension");
            PlayerRecord r = await OnlineByGameId(gameId) ?? throw new ActionFailure("player " + gameId + " is not online");
            var pos = new PVector3((float)x.Value, (float)y.Value, (float)z.Value);
            if (!string.IsNullOrEmpty(dim)) dim = PlayfieldOf(dim, r);
            if (!string.IsNullOrEmpty(dim) && !string.Equals(dim, r.Playfield, StringComparison.OrdinalIgnoreCase))
                await _game.Request(CmdId.Request_Player_ChangePlayerfield, new IdPlayfieldPositionRotation(r.EntityId, dim, pos, new PVector3(0, 0, 0)));
            else
                await _game.Request(CmdId.Request_Entity_Teleport, new IdPositionRotation(r.EntityId, pos, new PVector3(0, 0, 0)));
            return JValue.CreateNull();
        }

        // ---------------------------------------------------------------- moderation

        async Task<JToken> Kick(Args a)
        {
            string gameId = a.RequirePlayerGameId();
            PlayerRecord r = await OnlineByGameId(gameId) ?? throw new ActionFailure("player " + gameId + " is not online");
            string reason = Sanitize(a.Str("reason") ?? "Kicked");
            string outp = await RunConsole("kick " + r.SteamId + " '" + reason + "'");
            CheckConsoleFailure(outp, "kick");
            return JValue.CreateNull();
        }

        async Task<JToken> Ban(Args a)
        {
            string gameId = a.RequirePlayerGameId();
            PlayerRecord r = _players.Resolve(gameId);
            string steam = r != null ? r.SteamId : gameId;
            if (!PlayerRegistry.IsPlayerId(steam)) throw new ActionFailure("cannot ban '" + gameId + "': no player id known");
            string reason = a.Str("reason");
            DateTime? expires = ParseIso(a.Str("expiresAt"));
            string duration = BanDuration(expires, DateTime.UtcNow);
            string outp = await RunConsole("ban " + steam + " " + duration);
            CheckConsoleFailure(outp, "ban");
            _banReasons.Set(steam, reason, expires);
            if (r != null && r.Online)
            {
                try { await RunConsole("kick " + steam + " '" + Sanitize(reason ?? "Banned") + "'"); } catch (Exception) { }
            }
            return JValue.CreateNull();
        }

        /// <summary>The console takes hours/days ('2h', '14d'); no expiry is the longest the game accepts.</summary>
        public static string BanDuration(DateTime? expiresUtc, DateTime nowUtc)
        {
            if (expiresUtc == null) return "3650d";
            // The console only takes whole hours/days: round to the nearest hour (error <= 30 min); listBans reports
            // the game's real end time, so the stored expiry stays truthful.
            double hours = Math.Round((expiresUtc.Value - nowUtc).TotalHours, MidpointRounding.AwayFromZero);
            if (hours < 1) hours = 1;
            if (hours % 24 == 0) return ((int)(hours / 24)).ToString(CultureInfo.InvariantCulture) + "d";
            return ((int)hours).ToString(CultureInfo.InvariantCulture) + "h";
        }

        async Task<JToken> Unban(string gameId)
        {
            PlayerRecord r = _players.Resolve(gameId);
            string steam = r != null ? r.SteamId : gameId;
            string outp = await RunConsole("unban " + steam);
            CheckConsoleFailure(outp, "unban");
            _banReasons.Remove(steam);
            return JValue.CreateNull();
        }

        async Task<JToken> ListBans()
        {
            var data = (BannedPlayerData)await _game.Request(CmdId.Request_GetBannedPlayers, null);
            var arr = new JArray();
            foreach (BannedPlayerData.BanEntry e in data?.BannedPlayers ?? new List<BannedPlayerData.BanEntry>())
            {
                string steam = e.steam64Id.ToString(CultureInfo.InvariantCulture);
                PlayerRecord r = _players.BySteam(steam);
                DateTime until = DateTime.FromBinary(e.dateTime).ToUniversalTime();
                BanReasons.Entry known = _banReasons.Get(steam);
                bool permanent = known != null && known.ExpiresUtc == null;
                arr.Add(new JObject
                {
                    ["player"] = PlayerRegistry.Identity(steam, r?.Name),
                    ["reason"] = known?.Reason,
                    ["expiresAt"] = permanent ? null : until.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture),
                });
            }
            return arr;
        }

        static void CheckConsoleFailure(string output, string what)
        {
            if (string.IsNullOrEmpty(output)) return;
            if (Regex.IsMatch(output, "not found|unknown|invalid|error|usage", RegexOptions.IgnoreCase))
                throw new ActionFailure(what + " failed: " + output.Trim());
        }

        static string Sanitize(string s) { return Regex.Replace(s ?? "", "['\"\\r\\n]", " ").Trim(); }

        static DateTime? ParseIso(string s)
        {
            if (string.IsNullOrEmpty(s)) return null;
            return DateTime.TryParse(s, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out DateTime d)
                ? d : (DateTime?)null;
        }
    }

    public sealed class UnknownActionException : Exception
    {
        public UnknownActionException(string action) : base("unknown action '" + action + "'") { }
    }

    /// <summary>Texts the connector itself sent to chat, so the chat listener never re-ingests them.</summary>
    public static class ServerEcho
    {
        static readonly object Lock = new object();
        static readonly LinkedList<KeyValuePair<string, DateTime>> Recent = new LinkedList<KeyValuePair<string, DateTime>>();

        public static void Remember(string text)
        {
            lock (Lock)
            {
                Recent.AddLast(new KeyValuePair<string, DateTime>(text, DateTime.UtcNow));
                while (Recent.Count > 50) Recent.RemoveFirst();
            }
        }

        public static bool IsOurs(string text)
        {
            lock (Lock)
            {
                DateTime cutoff = DateTime.UtcNow.AddSeconds(-30);
                return Recent.Any(kv => kv.Value > cutoff && kv.Key == text);
            }
        }
    }

    /// <summary>The game's ban store keeps only SteamID + end time; reasons and "permanent" are kept here.</summary>
    public sealed class BanReasons
    {
        public sealed class Entry { public string Reason; public DateTime? ExpiresUtc; }

        readonly string _path;
        readonly Dictionary<string, Entry> _map;
        readonly object _lock = new object();

        public BanReasons(string path)
        {
            _path = path;
            try { _map = path != null && File.Exists(path) ? Newtonsoft.Json.JsonConvert.DeserializeObject<Dictionary<string, Entry>>(File.ReadAllText(path)) : null; }
            catch (Exception) { _map = null; }
            if (_map == null) _map = new Dictionary<string, Entry>();
        }

        public void Set(string steam, string reason, DateTime? expires) { lock (_lock) { _map[steam] = new Entry { Reason = reason, ExpiresUtc = expires }; Save(); } }
        public void Remove(string steam) { lock (_lock) { if (_map.Remove(steam)) Save(); } }
        public Entry Get(string steam) { lock (_lock) return _map.TryGetValue(steam, out Entry e) ? e : null; }

        void Save()
        {
            if (_path == null) return;
            try { File.WriteAllText(_path, Newtonsoft.Json.JsonConvert.SerializeObject(_map, Newtonsoft.Json.Formatting.Indented)); } catch (Exception) { }
        }
    }
}
