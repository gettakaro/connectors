using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Map
{
    /// <summary>
    /// The protocol v1 map: one Dimension per planet whose terrain the dedi has generated (its cache folder holds
    /// terrain.raw), tiles rendered from those files on worker threads and kept in a bounded cache. Never touches
    /// the game thread or the game API.
    /// </summary>
    public sealed class MapService
    {
        /// <summary>Takaro accepts at most 16 Dimensions.</summary>
        public const int MaxDimensions = 16;
        public long CacheBytes = 32L * 1024 * 1024;

        readonly Func<string> _playfieldsDir;
        readonly Func<string> _templatesDir;
        readonly ILog _log;
        readonly object _lock = new object();
        List<Planet> _planets = new List<Planet>();
        readonly Dictionary<string, Renderer> _renderers = new Dictionary<string, Renderer>();
        readonly Dictionary<string, object> _buildLocks = new Dictionary<string, object>();
        readonly Dictionary<string, LinkedListNode<KeyValuePair<string, byte[]>>> _tiles = new Dictionary<string, LinkedListNode<KeyValuePair<string, byte[]>>>();
        readonly LinkedList<KeyValuePair<string, byte[]>> _lru = new LinkedList<KeyValuePair<string, byte[]>>();
        long _cached;
        readonly HashSet<string> _reported = new HashSet<string>();

        public MapService(Func<string> playfieldsDir, Func<string> templatesDir, ILog log)
        {
            _playfieldsDir = playfieldsDir; _templatesDir = templatesDir; _log = log;
        }

        public IReadOnlyList<Planet> Planets { get { lock (_lock) return _planets; } }

        /// <summary>
        /// The dedi's terrain cache for this save: Saves/Cache/&lt;game&gt;/Playfields. AppFolder.Cache may name the
        /// cache root or the save's own cache folder, so both are tried, then the path next to the save.
        /// </summary>
        public static string FindPlayfieldsDir(string cacheFolder, string saveGameDir)
        {
            string save = string.IsNullOrEmpty(saveGameDir) ? null : Path.GetFileName(saveGameDir.TrimEnd('/', '\\'));
            var candidates = new List<string>();
            if (!string.IsNullOrEmpty(cacheFolder))
            {
                candidates.Add(Path.Combine(cacheFolder, "Playfields"));
                if (save != null) candidates.Add(Path.Combine(Path.Combine(cacheFolder, save), "Playfields"));
            }
            if (save != null)
            {
                string saves = Path.GetDirectoryName(Path.GetDirectoryName(saveGameDir.TrimEnd('/', '\\')));
                if (saves != null) candidates.Add(Path.Combine(Path.Combine(Path.Combine(saves, "Cache"), save), "Playfields"));
            }
            return candidates.FirstOrDefault(Directory.Exists);
        }

        /// <summary>Re-reads which planets exist. Returns true when the Dimension set changed.</summary>
        public bool Rescan()
        {
            string dir = _playfieldsDir();
            string templates = _templatesDir();
            var found = new List<Planet>();
            if (dir != null && Directory.Exists(dir))
            {
                foreach (string pf in Directory.GetDirectories(dir))
                {
                    if (!File.Exists(Path.Combine(pf, "terrain.raw"))) continue; // space playfields have no terrain
                    Planet p = Planet.Probe(pf, templates, out string why);
                    if (p == null)
                    {
                        if (_reported.Add(pf + why)) _log.Warn("map: skipping playfield " + Path.GetFileName(pf) + ": " + why);
                        continue;
                    }
                    found.Add(p);
                }
            }
            // Most recently generated first, so the playfields players use stay inside Takaro's limit.
            found = found.OrderByDescending(p => File.GetLastWriteTimeUtc(Path.Combine(p.Dir, "terrain.raw"))).ThenBy(p => p.Name, StringComparer.Ordinal).ToList();
            if (found.Count > MaxDimensions)
            {
                string dropped = string.Join(", ", found.Skip(MaxDimensions).Select(p => p.Name));
                if (_reported.Add("limit:" + dropped)) _log.Warn("map: Takaro shows at most " + MaxDimensions + " maps; leaving out " + dropped);
                found = found.Take(MaxDimensions).ToList();
            }
            found = found.OrderBy(p => p.Name, StringComparer.Ordinal).ToList();
            // Two names that sanitize to one id would collide; the second is left out.
            found = found.GroupBy(p => p.Id, StringComparer.Ordinal).Select(g => g.First()).ToList();

            lock (_lock)
            {
                bool changed = found.Count != _planets.Count
                               || found.Where((p, i) => p.Id != _planets[i].Id || p.Stamp != _planets[i].Stamp || p.WidthPx != _planets[i].WidthPx).Any();
                if (!changed) return false;
                foreach (Planet p in found)
                {
                    Planet old = _planets.FirstOrDefault(o => o.Id == p.Id);
                    if (old != null && old.Stamp != p.Stamp) Invalidate(p.Id);
                }
                _planets = found;
            }
            _log.Info("map: " + found.Count + " planet map(s): " + string.Join(", ", found.Select(p => p.Name + " (" + p.WorldWidth + "x" + p.WorldHeight + " m, zoom 0-" + p.MaxZoom + ")")));
            return true;
        }

        void Invalidate(string id)
        {
            _renderers.Remove(id);
            foreach (string key in _tiles.Keys.Where(k => k.StartsWith(id + "/", StringComparison.Ordinal)).ToList())
            {
                _cached -= _tiles[key].Value.Value.Length;
                _lru.Remove(_tiles[key]);
                _tiles.Remove(key);
            }
        }

        /// <summary>The `map` Capability, or null when no planet has terrain yet (Takaro needs 1 to 16 Dimensions).</summary>
        public JObject Capability()
        {
            List<Planet> planets;
            lock (_lock) planets = _planets;
            if (planets.Count == 0) return null;
            return new JObject
            {
                ["name"] = "map",
                ["parameters"] = new JObject { ["dimensions"] = new JArray(planets.Select(p => (JToken)p.ToDimension())) },
            };
        }

        public Planet Find(string dimensionId)
        {
            lock (_lock) return _planets.FirstOrDefault(p => p.Id == dimensionId);
        }

        /// <summary>A PNG tile, or null when the tile lies outside the planet (Takaro shows it transparent).</summary>
        public byte[] Tile(string dimensionId, int zoom, int x, int y)
        {
            Planet p = Find(dimensionId) ?? throw new MapNotFoundException("no map for dimension '" + dimensionId + "'");
            if (zoom < 0 || zoom > p.MaxZoom || x < 0 || y < 0 || x >= (1 << zoom) || y >= (1 << zoom))
                throw new ArgumentOutOfRangeException("tile", "tile " + zoom + "/" + x + "/" + y + " is outside zoom 0-" + p.MaxZoom);
            string key = p.Id + "/" + zoom + "/" + x + "/" + y;
            lock (_lock)
            {
                if (_tiles.TryGetValue(key, out var node))
                {
                    _lru.Remove(node);
                    _lru.AddFirst(node);
                    return node.Value.Value.Length == 0 ? null : node.Value.Value;
                }
            }
            Renderer r = RendererFor(p);
            byte[] rgba = r.RenderTile(zoom, x, y, Planet.TileSize);
            byte[] png = rgba == null ? new byte[0] : Png.EncodeRgba(rgba, Planet.TileSize, Planet.TileSize);
            lock (_lock)
            {
                if (!_tiles.ContainsKey(key))
                {
                    var node = _lru.AddFirst(new KeyValuePair<string, byte[]>(key, png));
                    _tiles[key] = node;
                    _cached += png.Length;
                    while (_cached > CacheBytes && _lru.Count > 1)
                    {
                        var last = _lru.Last;
                        _lru.RemoveLast();
                        _tiles.Remove(last.Value.Key);
                        _cached -= last.Value.Value.Length;
                    }
                }
            }
            return png.Length == 0 ? null : png;
        }

        /// <summary>The base image of a planet, built once (about a second for 1024 x 512) on the calling worker thread.</summary>
        public Renderer RendererFor(Planet p)
        {
            object gate;
            lock (_lock)
            {
                if (_renderers.TryGetValue(p.Id, out Renderer ready)) return ready;
                if (!_buildLocks.TryGetValue(p.Id, out gate)) _buildLocks[p.Id] = gate = new object();
            }
            lock (gate)
            {
                lock (_lock) if (_renderers.TryGetValue(p.Id, out Renderer ready)) return ready;
                var sw = System.Diagnostics.Stopwatch.StartNew();
                float[] heights = p.ReadHeights();
                BiomeMap biomes = p.ReadBiomes();
                if (biomes == null) _log.Warn("map: " + p.Name + " biomes.dat missing or unreadable; colouring by height only");
                Renderer r = Renderer.Build(p.WidthPx, p.HeightPx, heights, biomes, p.MetresPerPixel, p.SeaLevel);
                lock (_lock) _renderers[p.Id] = r;
                _log.Info("map: rendered " + p.Name + " base image " + p.WidthPx + "x" + p.HeightPx + " in " + sw.ElapsedMilliseconds + " ms");
                return r;
            }
        }
    }

    public sealed class MapNotFoundException : Exception
    {
        public MapNotFoundException(string message) : base(message) { }
    }
}
