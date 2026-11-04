using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Eleon.Modding;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Game
{
    /// <summary>
    /// Runs inside each PlayfieldServer process (ModTargets includes PfServer). Kill and death game events are
    /// raised only there, never on the dedi, so this agent forwards them to the dedi instance over the game's own
    /// mod network (INetwork.SendToDedicatedServer). It holds no Takaro connection and no secrets.
    /// </summary>
    public sealed class PlayfieldAgent
    {
        public const string Channel = "TakaroEmpyrion";

        readonly IModApi _api;
        readonly string _logPath;
        readonly bool _debug;
        IPlayfield _pf;
        int _lastHeldItem;   // runtime item id from the latest HoldingItem event on this playfield

        /// <summary>The playfield this process hosts: ClientPlayfield is null in a PfServer, OnPlayfieldLoaded is not.</summary>
        IPlayfield Pf => _pf ?? _api.ClientPlayfield;

        public PlayfieldAgent(IModApi api, string modDir, bool debug)
        {
            _api = api;
            _debug = debug;
            string logs = Path.Combine(modDir, "Logs");
            Directory.CreateDirectory(logs);
            _logPath = Path.Combine(logs, "playfield-" + System.Diagnostics.Process.GetCurrentProcess().Id + ".log");
        }

        public void Start()
        {
            _api.GameEvent += OnGameEvent;
            _api.Application.OnPlayfieldLoaded += p => { _pf = p; Log("playfield loaded " + p?.Name); };
            _api.Application.OnPlayfieldUnloading += p => { if (ReferenceEquals(_pf, p)) _pf = null; };
            Log("playfield agent started");
        }

        public void Stop() { try { _api.GameEvent -= OnGameEvent; } catch (Exception) { } }

        void OnGameEvent(GameEventType type, object a1, object a2, object a3, object a4, object a5)
        {
            try
            {
                if (type == GameEventType.HoldingItem && a1 is int held) _lastHeldItem = held;
                if (_debug) Log(type + " " + Describe(a1) + " | " + Describe(a2) + " | " + Describe(a3) + " | " + Describe(a4) + " | " + Describe(a5));
                if (type != GameEventType.KilledEntity && type != GameEventType.SubjectKilled && type != GameEventType.PlayerDied) return;
                if (_debug && Pf != null)
                    Log("context players=[" + string.Join(",", Pf.Players.Values.Select(pl => pl.Id + "=" + pl.Name + "/" + pl.SteamId)) + "] entities=" + Pf.Entities.Count);
                var msg = new JObject
                {
                    ["kind"] = type.ToString(),
                    ["playfield"] = Pf?.Name,
                    ["args"] = new JArray(Describe(a1), Describe(a2), Describe(a3), Describe(a4), Describe(a5)),
                };
                IPlayer local = FindPlayer(a1, a2, a3, a4, a5);
                object victim = null;
                if (local == null && type == GameEventType.KilledEntity)
                {
                    // KilledEntity(attacker, victim, ...) passes obfuscated game objects: match by their values, never by
                    // obfuscated member names (they change between builds).
                    foreach (object a in new[] { a1, a2 })
                    {
                        if (a == null || a is string || a.GetType().IsPrimitive) continue;
                        List<string> strings = StringValues(a);
                        if (_debug) Log("arg strings: " + string.Join(" | ", strings.Take(25)));
                        IPlayer p = Pf?.Players?.Values.FirstOrDefault(pl => !string.IsNullOrEmpty(pl.Name) && strings.Contains(pl.Name));
                        if (p != null && local == null) local = p;
                        else if (victim == null) { victim = a; msg["victimStrings"] = new JArray(strings.Distinct().Take(40)); }
                    }
                    // HoldingItem carries no player, so it names the weapon only when one player is on this playfield.
                    if (local != null && _lastHeldItem > 0 && Pf?.Players?.Count == 1) msg["heldItem"] = _lastHeldItem;
                }
                if (local != null)
                {
                    msg["playerEntityId"] = local.Id;
                    msg["steamId"] = local.SteamId;
                }
                string pf = Pf?.Name ?? "";
                bool sent = _api.Network.SendToDedicatedServer(Channel, Encoding.UTF8.GetBytes(msg.ToString(Formatting.None)), pf);
                Log("forwarded " + type + " to dedi: " + sent);
            }
            catch (Exception e) { Log("game event " + type + " failed: " + e.Message); }
        }

        /// <summary>The player the event is about: an IPlayer arg, or an entity id that is a player on this playfield.</summary>
        IPlayer FindPlayer(params object[] args)
        {
            foreach (object a in args)
            {
                if (a is IPlayer p) return p;
                if (a is int id && Pf != null && Pf.Players != null
                    && Pf.Players.TryGetValue(id, out IPlayer byId)) return byId;
            }
            return null;
        }

        static string Describe(object o)
        {
            if (o == null) return null;
            if (o is IPlayer p) return "player:" + p.Id + ":" + p.SteamId;
            if (o is IEntity e) return "entity:" + e.Id + ":" + e.Type + ":" + e.Name;
            if (o is string || o.GetType().IsPrimitive || o is Enum) return o.ToString();
            try { return o.GetType().Name + ":" + JsonConvert.SerializeObject(o); } catch (Exception) { return o.GetType().Name + "{" + Dump(o) + "}"; }
        }

        /// <summary>Non-empty string fields and properties of a game object (its own type and base types).</summary>
        static List<string> StringValues(object o)
        {
            var list = new List<string>();
            const System.Reflection.BindingFlags F = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic;
            for (Type t = o.GetType(); t != null && t != typeof(object); t = t.BaseType)
            {
                foreach (var f in t.GetFields(F))
                {
                    if (f.FieldType != typeof(string)) continue;
                    try { if (f.GetValue(o) is string v && v.Length > 0) list.Add(v); } catch (Exception) { }
                }
                foreach (var pr in t.GetProperties(F))
                {
                    if (pr.PropertyType != typeof(string) || pr.GetIndexParameters().Length > 0) continue;
                    try { if (pr.GetValue(o, null) is string v && v.Length > 0) list.Add(v); } catch (Exception) { }
                }
            }
            return list;
        }

        /// <summary>Debug: scalar fields and properties of an obfuscated game object, for mapping event payloads.</summary>
        static string Dump(object o)
        {
            var sb = new StringBuilder();
            const System.Reflection.BindingFlags F = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic;
            int n = 0;
            for (Type t = o.GetType(); t != null && t != typeof(object) && n < 60; t = t.BaseType)
            {
                foreach (var f in t.GetFields(F))
                {
                    if (n >= 60) break;
                    Type ft = f.FieldType;
                    if (!(ft.IsPrimitive || ft.IsEnum || ft == typeof(string) || ft.Name == "Vector3")) continue;
                    object v; try { v = f.GetValue(o); } catch (Exception) { continue; }
                    sb.Append(t.Name).Append('.').Append(f.Name).Append(':').Append(ft.Name).Append('=').Append(v).Append("; ");
                    n++;
                }
                foreach (var pr in t.GetProperties(F))
                {
                    if (n >= 60) break;
                    Type pt = pr.PropertyType;
                    if (pr.GetIndexParameters().Length > 0 || !(pt.IsPrimitive || pt.IsEnum || pt == typeof(string) || pt.Name == "Vector3")) continue;
                    object v; try { v = pr.GetValue(o, null); } catch (Exception) { continue; }
                    sb.Append(t.Name).Append(".P:").Append(pr.Name).Append(':').Append(pt.Name).Append('=').Append(v).Append("; ");
                    n++;
                }
            }
            return sb.ToString();
        }

        void Log(string s)
        {
            try { File.AppendAllText(_logPath, DateTime.UtcNow.ToString("o") + " " + s + Environment.NewLine); } catch (Exception) { }
        }
    }
}
