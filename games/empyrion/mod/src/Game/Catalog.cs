using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Game
{
    /// <summary>
    /// Item/block and creature catalogues with human display names. Codes are the game's config names
    /// (ItemsConfig/BlocksConfig "Name:"), which are stable across saves; ids come from the running game's
    /// GetBlockAndItemMapping() so scenario overrides are respected. Display names come from Localization.csv
    /// (active scenario first, then the base game).
    /// </summary>
    public sealed class Catalog
    {
        readonly Dictionary<string, int> _idByName = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<int, string> _nameById = new Dictionary<int, string>();
        readonly Dictionary<string, string> _display = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly HashSet<string> _itemNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly HashSet<string> _hidden = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly List<EntityDef> _entities = new List<EntityDef>();

        public sealed class EntityDef { public string Code; public string Faction; public bool Enemy; }

        public int Count { get { return _idByName.Count; } }
        public bool Loaded { get { return _idByName.Count > 0; } }

        public void LoadMapping(IDictionary<string, int> mapping)
        {
            _idByName.Clear(); _nameById.Clear();
            if (mapping == null) return;
            foreach (KeyValuePair<string, int> kv in mapping)
            {
                if (string.IsNullOrEmpty(kv.Key)) continue;
                _idByName[kv.Key] = kv.Value;
                if (!_nameById.ContainsKey(kv.Value)) _nameById[kv.Value] = kv.Key;
            }
        }

        /// <summary>
        /// Fallback when the dedi's GetBlockAndItemMapping() is empty: block ids are BlocksConfig "Id:", item ids are
        /// 8192 + ItemsConfig "Id:". Measured on v1.19.6 from a real starter inventory (8194 Flashlight = Id 2,
        /// 8231 SurvivalTool = Id 39, 8900 PDA = Id 708); ModConsts.ItemBaseId (4096) is NOT the runtime base and
        /// ids built from it crash the client's packet handler.
        /// </summary>
        public const int RuntimeItemBase = 8192;

        public int LoadMappingFromEcf(IEnumerable<string> contentDirs)
        {
            var map = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (string kind in new[] { "BlocksConfig.ecf", "ItemsConfig.ecf" })
            {
                string path = FindConfig(contentDirs, kind);
                if (path == null) continue;
                foreach (EcfBlock b in Ecf.Parse(File.ReadAllText(path)))
                {
                    if (string.IsNullOrEmpty(b.Name) || !b.Props.TryGetValue("Id", out string idText) || !int.TryParse(idText, out int id)) continue;
                    if (b.Kind == "Item") map[b.Name] = RuntimeItemBase + id;
                    else if (b.Kind == "Block") map[b.Name] = id;
                }
            }
            LoadMapping(map);
            return map.Count;
        }

        /// <summary>Names whose id differs between this catalogue and the runtime mapping (validates the fallback).</summary>
        public List<string> Mismatches(IDictionary<string, int> runtime)
        {
            var bad = new List<string>();
            foreach (KeyValuePair<string, int> kv in runtime)
                if (_idByName.TryGetValue(kv.Key, out int mine) && mine != kv.Value) bad.Add(kv.Key + " " + mine + "!=" + kv.Value);
            return bad;
        }

        static string FindConfig(IEnumerable<string> dirs, string file)
        {
            foreach (string d in dirs)
            {
                if (string.IsNullOrEmpty(d)) continue;
                foreach (string p in new[] { Path.Combine(d, "Content", "Configuration", file), Path.Combine(d, "Configuration", file) })
                    if (File.Exists(p)) return p;
            }
            return null;
        }

        /// <summary>Reads localization and config files; contentDirs ordered most specific first.</summary>
        public void LoadFiles(IEnumerable<string> contentDirs)
        {
            var dirs = contentDirs.Where(d => !string.IsNullOrEmpty(d) && Directory.Exists(d)).ToList();
            foreach (string d in Enumerable.Reverse(dirs))
            {
                string loc = Path.Combine(d, "Extras", "Localization.csv");
                if (File.Exists(loc)) LoadLocalization(File.ReadAllText(loc, Encoding.UTF8));
            }
            _hidden.Clear();
            foreach (string file in new[] { "ItemsConfig.ecf", "BlocksConfig.ecf" })
            {
                string path = FindConfig(dirs, file);
                if (path == null) continue;
                foreach (EcfBlock b in Ecf.Parse(File.ReadAllText(path)))
                {
                    if (string.IsNullOrEmpty(b.Name)) continue;
                    if (b.Kind == "Item") _itemNames.Add(b.Name);
                    // ShowUser: No marks internal entries players never see or hold.
                    if (b.Props.TryGetValue("ShowUser", out string show) && show.Equals("No", StringComparison.OrdinalIgnoreCase)) _hidden.Add(b.Name);
                }
            }
            foreach (string d in dirs)
            {
                string ecl = Path.Combine(d, "Content", "Configuration", "EClassConfig.ecf");
                if (!File.Exists(ecl)) ecl = Path.Combine(d, "Configuration", "EClassConfig.ecf");
                if (!File.Exists(ecl)) continue;
                LoadEntities(File.ReadAllText(ecl));
                break;
            }
        }

        public void LoadLocalization(string csv)
        {
            bool header = true;
            foreach (List<string> row in Csv.Rows(csv))
            {
                if (header) { header = false; continue; }
                if (row.Count < 2 || string.IsNullOrEmpty(row[0]) || string.IsNullOrEmpty(row[1])) continue;
                _display[row[0]] = Clean(row[1]);
            }
        }

        static readonly HashSet<string> StructureClasses = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { "ShipCV", "ShipSV", "ShipHV", "Base", "TurretAuto" };

        public int HiddenCount { get { return _hidden.Count; } }

        static readonly HashSet<string> HostileFactions = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { "Predator", "Zirax", "Kriel", "Alien", "Pirates", "Legacy", "TheLegacy" };

        public void LoadEntities(string ecf)
        {
            _entities.Clear();
            var blocks = Ecf.Parse(ecf).Where(x => x.Kind == "Entity" && !string.IsNullOrEmpty(x.Name)).ToList();
            var byName = new Dictionary<string, EcfBlock>(StringComparer.OrdinalIgnoreCase);
            foreach (EcfBlock x in blocks) byName[x.Name] = x;
            foreach (EcfBlock b in blocks)
            {
                // "Ref: Parent" inherits every property the block does not set itself (e.g. ParasaurBaby -> Parasaur02).
                string faction = Inherited(b, "Faction", byName);
                if (string.IsNullOrEmpty(faction) || faction.Equals("Player", StringComparison.OrdinalIgnoreCase)) continue;
                string cls = Inherited(b, "Class", byName);
                if (StructureClasses.Contains(cls ?? "")) continue;
                // Structures (BA_/CV_/SV_/HV_, chunk-cluster ships) and test rigs, also when only a Ref ancestor says so.
                if (Lineage(b, byName).Any(n => Regex.IsMatch(n, "^(BA|CV|SV|HV)_") || Regex.IsMatch(n, "Test(?=[A-Z_0-9]|$)"))) continue;
                if ((Inherited(b, "ModelType", byName) ?? "").StartsWith("ChunkCluster", StringComparison.OrdinalIgnoreCase)) continue;
                string enemy = Inherited(b, "IsEnemy", byName);
                bool isEnemy = enemy != null ? enemy.Equals("true", StringComparison.OrdinalIgnoreCase) : HostileFactions.Contains(faction);
                _entities.Add(new EntityDef { Code = b.Name, Faction = faction, Enemy = isEnemy });
            }
        }

        /// <summary>The block's name followed by its Ref ancestors, including a parent defined in another file.</summary>
        static IEnumerable<string> Lineage(EcfBlock b, Dictionary<string, EcfBlock> byName)
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string name = b.Name;
            EcfBlock cur = b;
            while (!string.IsNullOrEmpty(name) && seen.Add(name) && seen.Count <= 16)
            {
                yield return name;
                if (cur == null || !cur.Props.TryGetValue("Ref", out string parent)) yield break;
                name = parent;
                byName.TryGetValue(parent ?? "", out cur);
            }
        }

        /// <summary>A property from the block or, failing that, from its Ref chain (bounded, cycle-safe).</summary>
        static string Inherited(EcfBlock b, string key, Dictionary<string, EcfBlock> byName)
        {
            for (int hops = 0; b != null && hops < 16; hops++)
            {
                if (b.Props.TryGetValue(key, out string v) && !string.IsNullOrEmpty(v)) return v;
                if (!b.Props.TryGetValue("Ref", out string parent) || string.IsNullOrEmpty(parent) || !byName.TryGetValue(parent, out b)) return null;
            }
            return null;
        }

        static string Clean(string s)
        {
            s = Regex.Replace(s, "\\[[^\\]]*\\]", "");   // strip [b]/[c][ff0000] markup
            return s.Replace("\\n", " ").Trim();
        }

        public string DisplayName(string code)
        {
            if (string.IsNullOrEmpty(code)) return code;
            if (_display.TryGetValue(code, out string d) && d.Length > 0) return d;
            return Humanize(code);
        }

        /// <summary>"AssaultRifleEpic" -> "Assault Rifle Epic" when the game has no localization entry.</summary>
        public static string Humanize(string code)
        {
            string s = Regex.Replace(code, "(?<=[a-z])(?=[A-Z0-9])|(?<=[A-Z])(?=[A-Z][a-z])|_", " ");
            return Regex.Replace(s, "\\s+", " ").Trim();
        }

        public bool TryId(string codeOrNameOrId, out int id, out string code)
        {
            code = null; id = 0;
            if (string.IsNullOrEmpty(codeOrNameOrId)) return false;
            string key = codeOrNameOrId.Trim();
            if (_idByName.TryGetValue(key, out id)) { code = _nameById.TryGetValue(id, out string c) ? c : key; return true; }
            if (int.TryParse(key, out int n) && _nameById.TryGetValue(n, out code)) { id = n; return true; }
            // A display name (Takaro may send the item's name instead of its code).
            foreach (KeyValuePair<string, int> kv in _idByName)
                if (string.Equals(DisplayName(kv.Key), key, StringComparison.OrdinalIgnoreCase)) { id = kv.Value; code = kv.Key; return true; }
            return false;
        }

        public string CodeOf(int id) { return _nameById.TryGetValue(id, out string n) ? n : id.ToString(); }

        /// <summary>The first value that names a creature class in EClassConfig (case-insensitive), else null.</summary>
        public string FirstEntityCode(IEnumerable<string> candidates)
        {
            if (candidates == null) return null;
            foreach (string c in candidates)
            {
                if (string.IsNullOrEmpty(c)) continue;
                EntityDef d = _entities.FirstOrDefault(e => string.Equals(e.Code, c, StringComparison.OrdinalIgnoreCase));
                if (d != null) return d.Code;
            }
            return null;
        }

        /// <summary>Display name of a runtime item id (blocks keep their id, items are 8192 + config id), else null.</summary>
        public string ItemDisplayName(int runtimeId)
            => _nameById.TryGetValue(runtimeId, out string code) ? DisplayName(code) : null;

        public JArray ItemsJson()
        {
            var arr = new JArray();
            foreach (KeyValuePair<int, string> kv in _nameById.OrderBy(k => k.Key))
            {
                string code = kv.Value;
                if (kv.Key <= 0 || _hidden.Contains(code)) continue;
                arr.Add(new JObject
                {
                    ["code"] = code,
                    ["name"] = DisplayName(code),
                    ["description"] = _itemNames.Contains(code) ? "Item" : "Block",
                });
            }
            return arr;
        }

        public JArray EntitiesJson()
        {
            var arr = new JArray();
            foreach (EntityDef e in _entities)
            {
                var o = new JObject
                {
                    ["code"] = e.Code,
                    ["name"] = DisplayName(e.Code),
                    ["description"] = "Faction: " + e.Faction,
                    ["type"] = e.Enemy ? "hostile" : "friendly",
                };
                arr.Add(o);
            }
            return arr;
        }

        public int EntityCount { get { return _entities.Count; } }
    }

    public sealed class EcfBlock
    {
        public string Kind;
        public string Name;
        public Dictionary<string, string> Props = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>Minimal reader for Eleon's .ecf: top-level "{ [+]Kind Id: n, Name: X" blocks with "Key: value" lines.</summary>
    public static class Ecf
    {
        static readonly Regex Head = new Regex("^\\{\\s*\\+?(\\w+)\\s+(.*)$");

        public static IEnumerable<EcfBlock> Parse(string text)
        {
            EcfBlock cur = null;
            int depth = 0;
            foreach (string raw in text.Split('\n'))
            {
                string line = raw;
                int hash = line.IndexOf('#');
                if (hash >= 0) line = line.Substring(0, hash);
                line = line.Trim();
                if (line.Length == 0) continue;
                if (line.StartsWith("{"))
                {
                    depth++;
                    if (depth == 1)
                    {
                        Match m = Head.Match(line);
                        cur = new EcfBlock { Kind = m.Success ? m.Groups[1].Value : "" };
                        if (m.Success) ReadPairs(m.Groups[2].Value, cur);
                        cur.Props.TryGetValue("Name", out cur.Name);
                    }
                    if (line.EndsWith("}") && line.Length > 1) { depth--; if (depth == 0 && cur != null) { yield return cur; cur = null; } }
                    continue;
                }
                if (line.StartsWith("}"))
                {
                    depth--;
                    if (depth == 0 && cur != null) { yield return cur; cur = null; }
                    continue;
                }
                if (depth == 1 && cur != null) ReadPairs(line, cur);
            }
        }

        static void ReadPairs(string s, EcfBlock b)
        {
            // "Id: 3, Name: Pistol" or "Faction: Zirax"; values may be quoted and contain commas.
            foreach (Match m in Regex.Matches(s, "(\\w+)\\s*:\\s*(\"[^\"]*\"|[^,]*)"))
            {
                string k = m.Groups[1].Value;
                if (!b.Props.ContainsKey(k)) b.Props[k] = m.Groups[2].Value.Trim().Trim('"');
            }
        }
    }

    public static class Csv
    {
        public static IEnumerable<List<string>> Rows(string text)
        {
            var row = new List<string>();
            var cell = new StringBuilder();
            bool quoted = false;
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (quoted)
                {
                    if (c == '"') { if (i + 1 < text.Length && text[i + 1] == '"') { cell.Append('"'); i++; } else quoted = false; }
                    else cell.Append(c);
                    continue;
                }
                if (c == '"') { quoted = true; continue; }
                if (c == ',') { row.Add(cell.ToString()); cell.Clear(); continue; }
                if (c == '\r') continue;
                if (c == '\n') { row.Add(cell.ToString()); cell.Clear(); yield return row; row = new List<string>(); continue; }
                cell.Append(c);
            }
            if (cell.Length > 0 || row.Count > 0) { row.Add(cell.ToString()); yield return row; }
        }
    }
}
