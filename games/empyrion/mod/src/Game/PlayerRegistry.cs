using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Map;

namespace TakaroEmpyrion.Game
{
    public sealed class PlayerRecord
    {
        public string SteamId;      // game player id (PlayerInfo.steamId): SteamID64, or the game's numeric id for Epic players = Takaro gameId
        public string Name;
        public int EntityId;        // per-save handle, never sent as identity
        public bool Online;
        public string Playfield;
        public double X, Y, Z;
        public bool HasPosition;
        public int Ping;
        public string Ip;
        public DateTime LastSeenUtc;
    }

    /// <summary>
    /// Maps the game's entity ids to the stable SteamID64 and remembers last-known name/position, persisted in
    /// players.json, so getPlayer, unban and listBans work for offline players and disconnect events still carry
    /// the identity after the game dropped the entity.
    /// </summary>
    public sealed class PlayerRegistry
    {
        readonly object _lock = new object();
        readonly Dictionary<string, PlayerRecord> _bySteam = new Dictionary<string, PlayerRecord>();
        readonly Dictionary<int, string> _steamByEntity = new Dictionary<int, string>();
        readonly string _path;
        DateTime _lastSave = DateTime.MinValue;

        public PlayerRegistry(string path)
        {
            _path = path;
            try
            {
                if (path != null && File.Exists(path))
                    foreach (PlayerRecord r in JsonConvert.DeserializeObject<List<PlayerRecord>>(File.ReadAllText(path)) ?? new List<PlayerRecord>())
                    {
                        r.Online = false;
                        if (string.IsNullOrEmpty(r.SteamId)) continue;
                        _bySteam[r.SteamId] = r;
                        if (r.EntityId > 0) _steamByEntity[r.EntityId] = r.SteamId;
                    }
            }
            catch (Exception) { }
        }

        public PlayerRecord Upsert(string steamId, int entityId, string name)
        {
            if (string.IsNullOrEmpty(steamId)) return null;
            lock (_lock)
            {
                if (!_bySteam.TryGetValue(steamId, out PlayerRecord r)) { r = new PlayerRecord { SteamId = steamId }; _bySteam[steamId] = r; }
                if (entityId > 0) { r.EntityId = entityId; _steamByEntity[entityId] = steamId; }
                if (!string.IsNullOrEmpty(name)) r.Name = name;
                r.LastSeenUtc = DateTime.UtcNow;
                return r;
            }
        }

        public void SetPosition(PlayerRecord r, string playfield, double x, double y, double z)
        {
            lock (_lock) { r.Playfield = playfield; r.X = x; r.Y = y; r.Z = z; r.HasPosition = true; }
        }

        public void SetOnline(PlayerRecord r, bool online)
        {
            lock (_lock) { r.Online = online; r.LastSeenUtc = DateTime.UtcNow; }
            SaveSoon(force: true);
        }

        public PlayerRecord BySteam(string steamId)
        {
            if (steamId == null) return null;
            lock (_lock) return _bySteam.TryGetValue(steamId, out PlayerRecord r) ? r : null;
        }

        public PlayerRecord ByEntity(int entityId)
        {
            lock (_lock) return _steamByEntity.TryGetValue(entityId, out string s) && _bySteam.TryGetValue(s, out PlayerRecord r) ? r : null;
        }

        /// <summary>Resolves a Takaro gameId (SteamID64), and tolerates a display name as a last resort.</summary>
        public PlayerRecord Resolve(string gameId)
        {
            if (string.IsNullOrEmpty(gameId)) return null;
            PlayerRecord r = BySteam(gameId);
            if (r != null) return r;
            lock (_lock)
                return _bySteam.Values.FirstOrDefault(p => string.Equals(p.Name, gameId, StringComparison.OrdinalIgnoreCase));
        }

        public List<PlayerRecord> Online()
        {
            lock (_lock) return _bySteam.Values.Where(p => p.Online).ToList();
        }

        public List<PlayerRecord> All()
        {
            lock (_lock) return _bySteam.Values.ToList();
        }

        public void MarkAllOffline()
        {
            lock (_lock) foreach (PlayerRecord r in _bySteam.Values) r.Online = false;
        }

        public void SaveSoon(bool force = false)
        {
            if (_path == null) return;
            if (!force && DateTime.UtcNow - _lastSave < TimeSpan.FromSeconds(30)) return;
            string json;
            lock (_lock) { json = JsonConvert.SerializeObject(_bySteam.Values.ToList(), Formatting.Indented); _lastSave = DateTime.UtcNow; }
            try { File.WriteAllText(_path + ".tmp", json); if (File.Exists(_path)) File.Delete(_path); File.Move(_path + ".tmp", _path); }
            catch (Exception) { }
        }

        /// <summary>
        /// True for a SteamID64 (individual account, public universe): the top 32 bits are 0x01100001. Epic players
        /// carry a game-made numeric id (a hash of the Epic account id) that the game keeps out of this range.
        /// </summary>
        public static bool IsSteamId(string id)
            => ulong.TryParse(id, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out ulong v)
               && (v >> 32) == 0x01100001UL;

        /// <summary>Any id the game accepts as a player id (console ban/kick/unban): a plain unsigned number.</summary>
        public static bool IsPlayerId(string id)
            => ulong.TryParse(id, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out ulong _);

        /// <summary>
        /// Identity fields for a game player id. Steam players get steamId + platformId steam:&lt;id&gt;. Epic players get
        /// platformId epic:&lt;id&gt; only: the id is the game's own hash, not an EOS or Epic account id, so it must never
        /// be sent as steamId or epicOnlineServicesId.
        /// </summary>
        public static JObject Identity(string playerId, string name)
        {
            var o = new JObject { ["gameId"] = playerId, ["name"] = string.IsNullOrEmpty(name) ? playerId : name };
            if (IsSteamId(playerId)) { o["steamId"] = playerId; o["platformId"] = "steam:" + playerId; }
            else o["platformId"] = "epic:" + playerId;
            return o;
        }

        /// <summary>IGamePlayer: gameId + name, platform identity (see Identity), ping, ip (from the console plys).</summary>
        public static JObject ToTakaro(PlayerRecord r)
        {
            JObject o = Identity(r.SteamId, r.Name);
            if (r.Online && r.Ping > 0) o["ping"] = r.Ping;
            if (r.Online && !string.IsNullOrEmpty(r.Ip)) o["ip"] = r.Ip;
            return o;
        }

        public static JObject PositionOf(PlayerRecord r)
        {
            if (r == null || !r.HasPosition) return null;
            var p = new JObject { ["x"] = r.X, ["y"] = r.Y, ["z"] = r.Z };
            // The playfield as a map Dimension id (equal to the name unless it holds spaces or other symbols).
            if (!string.IsNullOrEmpty(r.Playfield)) p["dimension"] = Planet.DimensionId(r.Playfield);
            return p;
        }
    }
}
