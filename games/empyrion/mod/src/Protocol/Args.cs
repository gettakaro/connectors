using System;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Protocol
{
    /// <summary>
    /// Normalises Takaro request args. Takaro sends them as a JSON-encoded string ("{}"), but [] , {} ,
    /// null and a plain object all occur, and modules send explicit JSON null for optional fields.
    /// Player-bound actions carry either a flat gameId or a nested player object (sometimes the whole
    /// PlayerOnGameserver row with player.gameId inside).
    /// </summary>
    public sealed class Args
    {
        public readonly JObject Raw;

        Args(JObject raw) { Raw = raw ?? new JObject(); }

        public static Args Parse(JToken args)
        {
            if (args == null || args.Type == JTokenType.Null || args.Type == JTokenType.Undefined)
                return new Args(null);
            if (args.Type == JTokenType.String)
            {
                string s = ((string)args ?? "").Trim();
                if (s.Length == 0) return new Args(null);
                JToken parsed;
                try { parsed = ParseNoDates(s); }
                catch (Exception) { throw new ArgsException("args is not valid JSON"); }
                return Parse(parsed);
            }
            if (args.Type == JTokenType.Object) return new Args((JObject)args);
            if (args.Type == JTokenType.Array) return new Args(null);
            throw new ArgsException("args has unsupported type " + args.Type);
        }

        /// <summary>Keeps ISO timestamps (banPlayer expiresAt) as strings instead of Newtonsoft Date tokens.</summary>
        static JToken ParseNoDates(string s)
        {
            using (var reader = new Newtonsoft.Json.JsonTextReader(new System.IO.StringReader(s)) { DateParseHandling = Newtonsoft.Json.DateParseHandling.None })
                return JToken.ReadFrom(reader);
        }

        static bool Missing(JToken t) { return t == null || t.Type == JTokenType.Null || t.Type == JTokenType.Undefined; }

        public string Str(string key)
        {
            JToken t = Raw[key];
            if (Missing(t)) return null;
            if (t.Type == JTokenType.String) return (string)t;
            if (t.Type == JTokenType.Integer || t.Type == JTokenType.Float || t.Type == JTokenType.Boolean) return t.ToString();
            if (t.Type == JTokenType.Date)
                return ((DateTime)t).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ", System.Globalization.CultureInfo.InvariantCulture);
            return null;
        }

        public double? Num(string key)
        {
            JToken t = Raw[key];
            if (Missing(t)) return null;
            if (t.Type == JTokenType.Integer || t.Type == JTokenType.Float) return (double)t;
            if (t.Type == JTokenType.String && double.TryParse((string)t, System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture, out double d)) return d;
            return null;
        }

        public JObject Obj(string key) { return Raw[key] as JObject; }

        /// <summary>The target player's gameId: flat gameId, player.gameId, or player.player.gameId.</summary>
        public string PlayerGameId()
        {
            string flat = Str("gameId");
            if (!string.IsNullOrEmpty(flat)) return flat;
            JObject p = Obj("player");
            if (p != null)
            {
                string g = Missing(p["gameId"]) ? null : p["gameId"].ToString();
                if (!string.IsNullOrEmpty(g)) return g;
                JObject inner = p["player"] as JObject;
                if (inner != null && !Missing(inner["gameId"])) return inner["gameId"].ToString();
            }
            return null;
        }

        public string RequirePlayerGameId()
        {
            string id = PlayerGameId();
            if (string.IsNullOrEmpty(id)) throw new ArgsException("missing player gameId");
            return id;
        }
    }

    public sealed class ArgsException : Exception
    {
        public ArgsException(string message) : base(message) { }
    }
}
