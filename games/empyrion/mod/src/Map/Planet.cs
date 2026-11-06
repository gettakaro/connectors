using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Map
{
    /// <summary>
    /// One planet playfield as Takaro sees it: a map Dimension whose frame comes from the game's own files.
    ///
    /// Measured on the v1.19 dedi (Akua, Masperon, Ningues, Skillon):
    /// - Saves/Cache/&lt;game&gt;/Playfields/&lt;pf&gt;/terrain.raw is little-endian uint16, W x H with W = 2H
    ///   (1048576 bytes = 1024 x 512), row-major, row 0 = southmost (lowest z), column 0 = westmost (lowest x).
    ///   Height in metres = raw / 65535 * HeightMapMax (terrain.ecf).
    /// - The planet is 8192 * 2^(PlanetSize-2) by 4096 * 2^(PlanetSize-2) metres (the game's own formula;
    ///   PlanetSize from Templates/&lt;pf&gt;/playfield.yaml, absent = 2), centred on 0: column = (x + W/2) / m,
    ///   row = (z + H/2) / m with m metres per heightmap pixel. On Akua (size 2) m = 8, and ten logged player
    ///   positions sit 0.50 m above the sampled terrain with a spread of 0.02 m.
    /// </summary>
    public sealed class Planet
    {
        public const int TileSize = 256;
        /// <summary>Zoom levels past the heightmap's native resolution (each doubles it, interpolated).</summary>
        public const int OverZoom = 2;

        public string Name;
        public string Id;
        public string Dir;
        public int WidthPx, HeightPx;
        public int PlanetSize;
        public double HeightMapMax;
        public int SeaLevel;
        public long Stamp;

        public double WorldWidth { get { return 8192.0 * Math.Pow(2, PlanetSize - 2); } }
        public double WorldHeight { get { return WorldWidth / 2; } }
        public double MetresPerPixel { get { return WorldWidth / WidthPx; } }

        /// <summary>Zoom at which one tile pixel is one heightmap pixel (1024 px wide: zoom 2).</summary>
        public int NativeZoom { get { return Math.Max(0, (int)Math.Round(Math.Log(WidthPx / (double)TileSize, 2))); } }
        public int MaxZoom { get { return NativeZoom + OverZoom; } }

        /// <summary>Takaro ids allow [A-Za-z0-9_.:-]; playfield names can hold spaces ("Ashon II Moon").</summary>
        public static string DimensionId(string playfield)
        {
            if (string.IsNullOrEmpty(playfield)) return playfield;
            string id = Regex.Replace(playfield.Trim(), "[^A-Za-z0-9_.:-]", "_");
            if (id.Length == 0 || !char.IsLetterOrDigit(id[0])) id = "p" + id;
            return id.Length > 64 ? id.Substring(0, 64) : id;
        }

        /// <summary>The declared Dimension: x right, z up (north at the top), bounds = the whole planet.</summary>
        public JObject ToDimension()
        {
            double hw = WorldWidth / 2, hh = WorldHeight / 2;
            string name = Name.Trim();
            return new JObject
            {
                ["id"] = Id,
                ["name"] = name.Length > 64 ? name.Substring(0, 64) : name,
                ["axes"] = new JObject
                {
                    ["horizontal"] = new JObject { ["field"] = "x", ["direction"] = "right" },
                    ["vertical"] = new JObject { ["field"] = "z", ["direction"] = "up" },
                },
                ["bounds"] = new JObject
                {
                    ["horizontal"] = new JObject { ["min"] = -hw, ["max"] = hw },
                    ["vertical"] = new JObject { ["min"] = -hh, ["max"] = hh },
                },
                ["source"] = new JObject { ["type"] = "tiled", ["tileSize"] = TileSize, ["minZoom"] = 0, ["maxZoom"] = MaxZoom },
            };
        }

        /// <summary>World x/z to the heightmap's continuous pixel coordinates (column, row; row 0 = south).</summary>
        public void WorldToRaster(double x, double z, out double col, out double row)
        {
            col = (x + WorldWidth / 2) / MetresPerPixel;
            row = (z + WorldHeight / 2) / MetresPerPixel;
        }

        /// <summary>
        /// World x/z to Takaro's XYZ tile frame: the square of side WorldWidth anchored at the top-left
        /// (x = -W/2, z = +H/2), so the planet fills the upper half of the zoom-0 tile.
        /// </summary>
        public void WorldToTile(double x, double z, int zoom, out int tileX, out int tileY, out double px, out double py)
        {
            double perTile = WorldWidth / (1 << zoom);
            double gx = (x + WorldWidth / 2) / perTile, gy = (WorldHeight / 2 - z) / perTile;
            tileX = (int)Math.Floor(gx); tileY = (int)Math.Floor(gy);
            px = (gx - tileX) * TileSize; py = (gy - tileY) * TileSize;
        }

        // ------------------------------------------------------------------ discovery

        /// <summary>Reads the frame of one cached playfield, or null (with why) when it has no usable heightmap.</summary>
        public static Planet Probe(string playfieldDir, string templatesDir, out string why)
        {
            why = null;
            string name = Path.GetFileName(playfieldDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            var raw = new FileInfo(Path.Combine(playfieldDir, "terrain.raw"));
            if (!raw.Exists) { why = "no terrain.raw"; return null; }
            long pixels = raw.Length / 2;
            int w = (int)Math.Round(Math.Sqrt(pixels * 2));
            if (raw.Length % 2 != 0 || (long)w * (w / 2) != pixels || w < TileSize)
            {
                why = "terrain.raw is " + raw.Length + " bytes, not a W x W/2 uint16 heightmap";
                return null;
            }
            string tdir = templatesDir == null ? null : Path.Combine(templatesDir, name);
            string yaml = tdir == null ? null : ReadText(Path.Combine(tdir, "playfield.yaml"));
            if (yaml == null) { why = "no Templates/" + name + "/playfield.yaml (planet size unknown)"; return null; }
            string ecf = ReadText(Path.Combine(tdir, "terrain.ecf"));
            var p = new Planet
            {
                Name = name,
                Id = DimensionId(name),
                Dir = playfieldDir,
                WidthPx = w,
                HeightPx = w / 2,
                PlanetSize = YamlInt(yaml, "PlanetSize") ?? 2,
                HeightMapMax = EcfNumber(ecf, "HeightMapMax") ?? YamlInt(yaml, "TerrainHeightMapMax") ?? 500,
                SeaLevel = YamlInt(yaml, "SeaLevel") ?? 0,
                Stamp = raw.LastWriteTimeUtc.Ticks ^ raw.Length,
            };
            if (p.PlanetSize < 2 || p.PlanetSize > 12) { why = "PlanetSize " + p.PlanetSize + " out of range"; return null; }
            return p;
        }

        static string ReadText(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path) : null; } catch (Exception) { return null; }
        }

        /// <summary>A top-level "Key: 12" line (playfield.yaml).</summary>
        public static int? YamlInt(string yaml, string key)
        {
            if (yaml == null) return null;
            Match m = Regex.Match(yaml, "^" + key + "[ \\t]*:[ \\t]*(-?\\d+)", RegexOptions.Multiline);
            return m.Success ? int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) : (int?)null;
        }

        /// <summary>A 'Key: "490"' property (terrain.ecf).</summary>
        public static double? EcfNumber(string ecf, string key)
        {
            if (ecf == null) return null;
            Match m = Regex.Match(ecf, "^[ \\t]*" + key + "[ \\t]*:[ \\t]*\"?([0-9.]+)", RegexOptions.Multiline);
            return m.Success && double.TryParse(m.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out double v) ? v : (double?)null;
        }

        // ------------------------------------------------------------------ files

        /// <summary>terrain.raw as heights in metres, row 0 = south.</summary>
        public float[] ReadHeights()
        {
            byte[] b = File.ReadAllBytes(Path.Combine(Dir, "terrain.raw"));
            if (b.Length != WidthPx * HeightPx * 2) throw new InvalidDataException("terrain.raw changed size");
            var h = new float[WidthPx * HeightPx];
            float scale = (float)(HeightMapMax / 65535.0);
            for (int i = 0; i < h.Length; i++) h[i] = (b[2 * i] | (b[2 * i + 1] << 8)) * scale;
            return h;
        }

        /// <summary>biomes.dat, or null when absent or not in the measured layout.</summary>
        public BiomeMap ReadBiomes()
        {
            string path = Path.Combine(Dir, "biomes.dat");
            if (!File.Exists(path)) return null;
            return BiomeMap.Parse(File.ReadAllBytes(path), WidthPx * HeightPx);
        }
    }

    /// <summary>
    /// biomes.dat as measured (header 04 00 04 00 02 on all four cached planets): 5 header bytes, uint16 name
    /// count, then per biome a length-prefixed ASCII name and its uint16 id, then a block: byte 1, uint32 byte
    /// length, and (run, id) byte pairs that cover the same W x H grid as terrain.raw, same orientation. More
    /// data follows the block; it is not needed here.
    /// </summary>
    public sealed class BiomeMap
    {
        public readonly Dictionary<int, string> Names = new Dictionary<int, string>();
        public byte[] Cells;

        public static BiomeMap Parse(byte[] b, int cellCount)
        {
            try
            {
                int o = 5;
                int count = b[o] | (b[o + 1] << 8); o += 2;
                var map = new BiomeMap();
                for (int i = 0; i < count; i++)
                {
                    int len = 0, shift = 0;
                    byte lb;
                    do { lb = b[o++]; len |= (lb & 0x7F) << shift; shift += 7; } while ((lb & 0x80) != 0);
                    string name = Encoding.UTF8.GetString(b, o, len); o += len;
                    int id = b[o] | (b[o + 1] << 8); o += 2;
                    map.Names[id] = name;
                }
                if (b[o] != 1) return null;
                int bytes = BitConverter.ToInt32(b, o + 1); o += 5;
                if (bytes <= 0 || bytes % 2 != 0 || o + bytes > b.Length) return null;
                var cells = new byte[cellCount];
                int n = 0;
                for (int i = o; i < o + bytes; i += 2)
                {
                    int run = b[i];
                    if (n + run > cellCount) return null;
                    for (int k = 0; k < run; k++) cells[n++] = b[i + 1];
                }
                if (n != cellCount) return null;
                map.Cells = cells;
                return map;
            }
            catch (IndexOutOfRangeException) { return null; }
            catch (ArgumentException) { return null; }
        }
    }
}
