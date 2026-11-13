using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Eleon;
using Eleon.Modding;
using MsgChannel = Eleon.MsgChannel;
using SenderType = Eleon.SenderType;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Game
{
    /// <summary>
    /// Translates game signals into the six Takaro events. Sources: v1 Event_Player_Connected/Disconnected,
    /// v2 ChatMessageSent (typed sender + channel; v1 Event_ChatMessage is the fallback, deduplicated),
    /// v1 Event_Statistics(PlayerDied), v1 Event_GameEvent(KilledEntity), playfield load/unload as log lines.
    /// Handlers are called on the game thread and only queue work.
    /// </summary>
    public sealed class Events
    {
        readonly Actions _actions;
        readonly PlayerRegistry _players;
        readonly Catalog _catalog;
        readonly ILog _log;
        readonly Action<string, JObject> _emit;
        readonly Dictionary<string, DateTime> _recentChat = new Dictionary<string, DateTime>();
        readonly object _chatLock = new object();
        DateTime _logWindow = DateTime.UtcNow;
        int _logInWindow;
        public int LogEventsPerMinute = 30;

        public Events(Actions actions, PlayerRegistry players, Catalog catalog, ILog log, Action<string, JObject> emit)
        {
            _actions = actions; _players = players; _catalog = catalog; _log = log; _emit = emit;
        }

        public void OnLegacyEvent(CmdId id, object data)
        {
            switch (id)
            {
                case CmdId.Event_Player_Connected: Fire(() => Connected(((Id)data).id)); break;
                case CmdId.Event_Player_Disconnected: Fire(() => Disconnected(((Id)data).id)); break;
                case CmdId.Event_ChatMessage: OnLegacyChat((ChatInfo)data); break;
                case CmdId.Event_Statistics: OnStatistics((StatisticsParam)data); break;
                case CmdId.Event_GameEvent: OnGameEvent((GameEventData)data); break;
                case CmdId.Event_Playfield_Loaded:
                    var pl = data as PlayfieldLoad;
                    if (pl != null) Log("Playfield " + pl.playfield + " loaded in " + pl.sec.ToString("0.0") + "s");
                    break;
                case CmdId.Event_Playfield_Unloaded:
                    var pu = data as PlayfieldLoad;
                    if (pu != null) Log("Playfield " + pu.playfield + " unloaded");
                    break;
                case CmdId.Event_Player_ChangedPlayfield:
                    var cp = data as IdPlayfield;
                    if (cp != null)
                    {
                        PlayerRecord r = _players.ByEntity(cp.id);
                        if (r != null) r.Playfield = cp.playfield;
                    }
                    break;
            }
        }

        void Fire(Func<Task> work)
        {
            Task.Run(async () =>
            {
                try { await work(); }
                catch (Exception e) { _log.Warn("event handler failed: " + e.GetType().Name + ": " + e.Message); }
            });
        }

        // Joins whose player-connected is still being prepared: entity id -> join generation. A disconnect removes
        // the entry, so a join that ends before its identity is known sends neither event.
        readonly System.Collections.Concurrent.ConcurrentDictionary<int, long> _joining = new System.Collections.Concurrent.ConcurrentDictionary<int, long>();
        long _joinGen;

        async Task Connected(int entityId)
        {
            long gen = System.Threading.Interlocked.Increment(ref _joinGen);
            _joining[entityId] = gen;
            PlayerRecord r;
            try { r = await _actions.RefreshConnectingPlayer(entityId); }
            catch (Exception e)
            {
                // Never drop the join: send what is already known (Steam id and name) without ip/ping/position.
                r = await _actions.KnownPlayer(entityId);
                if (r == null)
                {
                    ((ICollection<KeyValuePair<int, long>>)_joining).Remove(new KeyValuePair<int, long>(entityId, gen));
                    _log.Error("player-connected for entity " + entityId + " not sent: no player id known (" + e.Message + ")");
                    return;
                }
                // ip and ping are only current when a getPlayers in this session already refreshed the record.
                if (!r.Online) { r.Ip = null; r.Ping = 0; }
                _log.Warn("player-connected for " + r.SteamId + " sent with the known id and name only: " + e.Message);
            }
            if (!((ICollection<KeyValuePair<int, long>>)_joining).Remove(new KeyValuePair<int, long>(entityId, gen)))
            {
                _log.Info("player-connected for entity " + entityId + " not sent: the player left, or joined again, before it was ready");
                return;
            }
            _players.SetOnline(r, true);
            var data = new JObject { ["player"] = PlayerRegistry.ToTakaro(r) };
            JObject pos = PlayerRegistry.PositionOf(r);
            if (pos != null) data["position"] = pos;
            _emit("player-connected", data);
        }

        Task Disconnected(int entityId)
        {
            if (_joining.TryRemove(entityId, out long _))
            {
                _log.Info("entity " + entityId + " left before player-connected was sent; sending neither event");
                return Task.CompletedTask;
            }
            PlayerRecord r = _players.ByEntity(entityId);
            // A refused login (ban, wrong password) disconnects entity -1, which never connected.
            if (r == null) { _log.Debug("disconnect of unknown entity " + entityId); return Task.CompletedTask; }
            _players.SetOnline(r, false);
            var data = new JObject { ["player"] = PlayerRegistry.ToTakaro(r) };
            JObject pos = PlayerRegistry.PositionOf(r);
            if (pos != null) data["position"] = pos;
            _emit("player-disconnected", data);
            return Task.CompletedTask;
        }

        // ------------------------------------------------------------- chat

        /// <summary>v2 ChatMessageSent: the typed source of truth for player chat.</summary>
        public void OnChat(MessageData m)
        {
            if (m == null) return;
            _log.Debug("chat v2 " + JsonConvert.SerializeObject(new { m.SenderType, m.Channel, m.SenderEntityId, m.RecipientEntityId, m.Text }));
            if (m.SenderType != SenderType.Player || m.SenderEntityId <= 0) return;
            string channel = MapChannel(m.Channel);
            if (channel == null) return;
            EmitChat(m.SenderEntityId, m.Text, channel, m.Channel == MsgChannel.SinglePlayer ? m.RecipientEntityId : -1);
        }

        void OnLegacyChat(ChatInfo c)
        {
            if (c == null) return;
            _log.Debug("chat v1 " + JsonConvert.SerializeObject(c));
            if (c.playerId <= 0) return;
            // Empyrion's ChatInfo.type follows MsgChannel + 3 (3 global, 5 faction, 8 private ...); be lenient.
            string channel = c.recipientEntityId > 0 ? "whisper" : c.recipientFactionId > 0 ? "team" : "global";
            Task.Delay(1500).ContinueWith(_ => EmitChat(c.playerId, c.msg, channel, c.recipientEntityId));
        }

        public static string MapChannel(MsgChannel ch)
        {
            switch (ch)
            {
                case MsgChannel.Global: return "global";
                case MsgChannel.Faction: return "team";
                case MsgChannel.Alliance: return "friends";
                case MsgChannel.SinglePlayer: return "whisper";
                default: return null; // Server channel = system messages
            }
        }

        void EmitChat(int senderEntity, string text, string channel, int recipientEntity)
        {
            if (string.IsNullOrEmpty(text) || ServerEcho.IsOurs(text)) return;
            string key = senderEntity + "\u0001" + text;
            lock (_chatLock)
            {
                DateTime now = DateTime.UtcNow;
                foreach (string k in _recentChat.Where(kv => now - kv.Value > TimeSpan.FromSeconds(5)).Select(kv => kv.Key).ToList()) _recentChat.Remove(k);
                if (_recentChat.ContainsKey(key)) return;
                _recentChat[key] = now;
            }
            Fire(async () =>
            {
                PlayerRecord r = _players.ByEntity(senderEntity) ?? await _actions.RefreshPlayer(senderEntity);
                var data = new JObject { ["player"] = PlayerRegistry.ToTakaro(r), ["channel"] = channel, ["msg"] = text };
                JObject pos = PlayerRegistry.PositionOf(r);
                if (pos != null) data["position"] = pos;
                if (channel == "whisper" && recipientEntity > 0)
                {
                    PlayerRecord to = _players.ByEntity(recipientEntity);
                    if (to != null) data["recipient"] = new JObject { ["gameId"] = to.SteamId, ["name"] = to.Name };
                }
                _emit("chat-message", data);
            });
        }

        // ------------------------------------------------------------- deaths and kills

        void OnStatistics(StatisticsParam s)
        {
            if (s == null) return;
            _log.Debug("statistics " + JsonConvert.SerializeObject(s));
            if (s.type != StatisticsType.PlayerDied) return;
            // PlayerDied, observed live 2026-11-03 (fall death): int1 = victim entity id, int2 = cause code (7),
            // int3 = -1, int4 = -1. int3 is taken as the killer entity id when it names a known player; PvP is not
            // live-verified yet (needs a second player).
            int victim = s.int1, killer = s.int3;
            Fire(async () =>
            {
                PlayerRecord v = _players.ByEntity(victim) ?? await _actions.RefreshPlayer(victim);
                try { v = await _actions.RefreshPlayer(victim); } catch (Exception) { }
                var data = new JObject { ["player"] = PlayerRegistry.ToTakaro(v) };
                if (killer > 0 && killer != victim)
                {
                    PlayerRecord k = _players.ByEntity(killer);
                    if (k != null) data["attacker"] = PlayerRegistry.ToTakaro(k);
                }
                JObject pos = PlayerRegistry.PositionOf(v);
                if (pos != null) data["position"] = pos;
                _emit("player-death", data);
            });
        }

        void OnGameEvent(GameEventData g)
        {
            _log.Debug("gameEvent " + JsonConvert.SerializeObject(new { type = (GameEventType)g.EventType, g.Name, g.Type, g.Amount, g.PlayerId, g.Flag }));
            if ((GameEventType)g.EventType != GameEventType.KilledEntity || g.PlayerId <= 0 || string.IsNullOrEmpty(g.Name)) return;
            Fire(async () =>
            {
                PlayerRecord r = _players.ByEntity(g.PlayerId) ?? await _actions.RefreshPlayer(g.PlayerId);
                await _actions.EnsureCatalog();
                // GameEventData.Type carries an item/block id for item events; whether KilledEntity puts the weapon
                // there is verified live. Never claim a weapon we did not read.
                string weapon = g.Type > 0 && _catalog.CodeOf(g.Type) != g.Type.ToString()
                    ? _catalog.DisplayName(_catalog.CodeOf(g.Type)) : "unknown";
                var data = new JObject
                {
                    ["player"] = PlayerRegistry.ToTakaro(r),
                    ["entity"] = _catalog.DisplayName(g.Name),
                    ["weapon"] = weapon,
                };
                JObject pos = PlayerRegistry.PositionOf(r);
                if (pos != null) data["position"] = pos;
                _emit("entity-killed", data);
            });
        }

        /// <summary>
        /// Kill/death game events forwarded by the PlayfieldAgent. Logged raw so the argument layout can be checked
        /// against live data; entity-killed is emitted only when both the player and the killed entity are named.
        /// </summary>
        public void OnPlayfieldPacket(string sender, string playfield, byte[] data)
        {
            string text;
            try { text = System.Text.Encoding.UTF8.GetString(data); } catch (Exception) { return; }
            _log.Info("playfield packet from " + sender + "@" + playfield + ": " + LogText.Clip(text, 500));
            JObject msg;
            try { msg = JObject.Parse(text); } catch (Exception) { return; }
            if ((string)msg["kind"] != "KilledEntity") return;
            int playerEntity = (int?)msg["playerEntityId"] ?? 0;
            string victim = null;
            foreach (JToken a in (JArray)msg["args"] ?? new JArray())
            {
                string s = (string)a;
                if (s == null || !s.StartsWith("entity:")) continue;
                string[] parts = s.Split(new[] { ':' }, 4);
                if (parts.Length == 4 && parts[2] != "Player") { victim = parts[3]; break; }
            }
            List<string> victimStrings = ((JArray)msg["victimStrings"])?.Select(t => (string)t).ToList();
            int heldItem = (int?)msg["heldItem"] ?? 0;
            if (playerEntity <= 0 || (victim == null && (victimStrings == null || victimStrings.Count == 0)))
            {
                _log.Info("KilledEntity skipped: no player or creature in the packet");
                return;
            }
            Fire(async () =>
            {
                // The catalogue loads lazily; resolve the creature class only once it is there.
                await _actions.EnsureCatalog();
                if (victim == null) victim = _catalog.FirstEntityCode(victimStrings);
                if (string.IsNullOrEmpty(victim)) { _log.Info("KilledEntity skipped: no known creature class in " + string.Join("|", victimStrings)); return; }
                PlayerRecord r = _players.ByEntity(playerEntity) ?? await _actions.RefreshPlayer(playerEntity);
                var ev = new JObject
                {
                    ["player"] = PlayerRegistry.ToTakaro(r),
                    ["entity"] = _catalog.DisplayName(victim),
                    ["weapon"] = (heldItem > 0 ? _catalog.ItemDisplayName(heldItem) : null) ?? "unknown",
                };
                JObject pos = PlayerRegistry.PositionOf(r);
                if (pos != null) ev["position"] = pos;
                _emit("entity-killed", ev);
            });
        }

        // ------------------------------------------------------------- log

        public void Log(string msg)
        {
            DateTime now = DateTime.UtcNow;
            if (now - _logWindow > TimeSpan.FromMinutes(1)) { _logWindow = now; _logInWindow = 0; }
            if (++_logInWindow > LogEventsPerMinute) return;
            _emit("log", new JObject { ["msg"] = msg });
        }
    }
}
