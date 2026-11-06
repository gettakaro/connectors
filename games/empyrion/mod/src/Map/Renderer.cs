using System;
using System.Collections.Generic;

namespace TakaroEmpyrion.Map
{
    /// <summary>
    /// Shaded relief from terrain.raw, tinted by biome. The base image (one RGB pixel per heightmap pixel,
    /// north up) is built once per planet; tiles are cut from it with bilinear sampling, wrapping east-west
    /// like the planet does.
    /// </summary>
    public sealed class Renderer
    {
        static readonly KeyValuePair<string[], byte[]>[] Rules =
        {
            Rule(new[] { "deepwater" }, 28, 52, 96),
            Rule(new[] { "water", "lake", "sea", "ocean", "lilies", "hotspring" }, 46, 86, 140),
            Rule(new[] { "pole", "polar", "glacier", "snow", "ice", "frost" }, 232, 236, 240),
            Rule(new[] { "beach", "shore", "sand", "dune", "drift" }, 206, 190, 142),
            Rule(new[] { "highmountain" }, 150, 146, 140),
            Rule(new[] { "lowmountain" }, 122, 114, 96),
            Rule(new[] { "mountain", "rock", "cliff", "plateau", "rift", "crater", "wadi", "technical" }, 136, 130, 120),
            Rule(new[] { "crash", "escape", "nest", "pod" }, 110, 100, 82),
            Rule(new[] { "lava", "volcan", "magma", "ash", "scorch", "burn" }, 70, 40, 32),
            Rule(new[] { "desert", "dustbowl", "dust", "savanna", "flats", "barren", "dry", "mesa", "canyon" }, 186, 140, 72),
            Rule(new[] { "swamp", "marsh", "morass", "peat", "bog", "fungus", "mud" }, 84, 92, 48),
            Rule(new[] { "taiga", "forest", "grove", "tree", "jungle", "wood", "plantation" }, 58, 92, 40),
            Rule(new[] { "tundra", "cold", "heath" }, 128, 136, 98),
            Rule(new[] { "oasis" }, 70, 128, 72),
            Rule(new[] { "ore", "cluster" }, 110, 100, 82),
            Rule(new[] { "grass", "meadow", "plain", "hill", "flower", "field", "steppe" }, 104, 132, 56),
        };

        static KeyValuePair<string[], byte[]> Rule(string[] keys, int r, int g, int b)
        {
            return new KeyValuePair<string[], byte[]>(keys, new[] { (byte)r, (byte)g, (byte)b });
        }

        /// <summary>
        /// The colour of a biome by its name; names differ per planet, so keywords decide, first match wins
        /// (order matters: "Crashsite" contains "ash", "Forest" contains "ore").
        /// </summary>
        public static byte[] BiomeColour(string name, out bool water)
        {
            string n = (name ?? "").ToLowerInvariant();
            water = false;
            for (int i = 0; i < Rules.Length; i++)
            {
                foreach (string k in Rules[i].Key)
                    if (n.Contains(k)) { water = i <= 1; return Rules[i].Value; }
            }
            return new byte[] { 118, 118, 100 };
        }

        public readonly int Width, Height;
        /// <summary>RGB, row 0 = north (top of the map): flipped row k is terrain.raw row Height-1-k.</summary>
        public readonly byte[] Rgb;

        public Renderer(int width, int height, byte[] rgb) { Width = width; Height = height; Rgb = rgb; }

        /// <summary>Builds the base image. heights: metres, row 0 = south (as in terrain.raw).</summary>
        public static Renderer Build(int w, int h, float[] heights, BiomeMap biomes, double metresPerPixel, int seaLevel)
        {
            var palette = new byte[256][];
            var isWater = new bool[256];
            if (biomes != null)
                foreach (KeyValuePair<int, string> kv in biomes.Names)
                    if (kv.Key >= 0 && kv.Key < 256) palette[kv.Key] = BiomeColour(kv.Value, out isWater[kv.Key]);

            // Light from the north-west, 45 degrees up; relief exaggerated 3x so 8 m pixels still show slopes.
            const double ex = 3.0;
            double lx = -1, ly = 1.4, lz = 1, ll = Math.Sqrt(lx * lx + ly * ly + lz * lz);
            lx /= ll; ly /= ll; lz /= ll;

            var rgb = new byte[w * h * 3];
            for (int r = 0; r < h; r++)
            {
                int rn = Math.Min(h - 1, r + 1), rs = Math.Max(0, r - 1);
                double dzSpan = (rn - rs) * metresPerPixel;
                int outRow = h - 1 - r;
                for (int c = 0; c < w; c++)
                {
                    int i = r * w + c;
                    double dx = (heights[r * w + (c + 1) % w] - heights[r * w + (c + w - 1) % w]) / (2 * metresPerPixel);
                    double dz = dzSpan > 0 ? (heights[rn * w + c] - heights[rs * w + c]) / dzSpan : 0;
                    double nx = -dx * ex, nz = -dz * ex, ny = 1;
                    double shade = (nx * lx + ny * ly + nz * lz) / Math.Sqrt(nx * nx + ny * ny + nz * nz) / ly;
                    shade = Math.Max(0.3, Math.Min(1.3, shade));

                    byte[] col;
                    bool water;
                    int id = biomes != null ? biomes.Cells[i] : -1;
                    if (id >= 0 && palette[id] != null) { col = palette[id]; water = isWater[id]; }
                    else { col = HeightColour(heights[i], seaLevel, out water); }
                    if (water) shade = 1 + 0.3 * (shade - 1);

                    int o = (outRow * w + c) * 3;
                    rgb[o] = Clamp(col[0] * shade);
                    rgb[o + 1] = Clamp(col[1] * shade);
                    rgb[o + 2] = Clamp(col[2] * shade);
                }
            }
            return new Renderer(w, h, rgb);
        }

        /// <summary>Without biomes: water below sea level, then a lowland-to-rock ramp.</summary>
        static byte[] HeightColour(float m, int seaLevel, out bool water)
        {
            water = m < seaLevel;
            if (water) return new byte[] { 46, 86, 140 };
            double t = Math.Max(0, Math.Min(1, (m - seaLevel) / 200.0));
            return new[] { (byte)(104 + t * 46), (byte)(132 + t * 14), (byte)(56 + t * 84) };
        }

        static byte Clamp(double v) { return (byte)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

        /// <summary>
        /// One RGBA tile of Takaro's XYZ frame (a square of side = planet width, top-left = north-west corner).
        /// Pixels south of the planet are transparent. Returns null when the whole tile is outside the planet.
        /// </summary>
        public byte[] RenderTile(int zoom, int tx, int ty, int tileSize)
        {
            int n = 1 << zoom;
            // Base-image pixels per tile pixel. The frame is Width pixels wide; the planet is Height (= Width/2) tall.
            double step = Width / (double)(n * tileSize);
            double y0 = (double)ty * tileSize * step;
            if (y0 >= Height) return null;
            double x0 = (double)tx * tileSize * step;
            int samples = step > 1 ? (int)Math.Ceiling(step) : 1;
            var outp = new byte[tileSize * tileSize * 4];
            for (int j = 0; j < tileSize; j++)
            {
                for (int i = 0; i < tileSize; i++)
                {
                    double r = 0, g = 0, b = 0;
                    int count = 0;
                    for (int sy = 0; sy < samples; sy++)
                    {
                        double fy = y0 + (j + (sy + 0.5) / samples) * step;
                        if (fy >= Height) continue;
                        for (int sx = 0; sx < samples; sx++)
                        {
                            double fx = x0 + (i + (sx + 0.5) / samples) * step;
                            Sample(fx, fy, ref r, ref g, ref b);
                            count++;
                        }
                    }
                    if (count == 0) continue;
                    int o = (j * tileSize + i) * 4;
                    outp[o] = Clamp(r / count);
                    outp[o + 1] = Clamp(g / count);
                    outp[o + 2] = Clamp(b / count);
                    outp[o + 3] = (byte)(count == samples * samples ? 255 : Clamp(255.0 * count / (samples * samples)));
                }
            }
            return outp;
        }

        /// <summary>
        /// Bilinear at a frame coordinate (fx east from x = -W/2, fy south from z = +H/2, in heightmap pixels); x wraps.
        /// The game samples heightmap value k at exactly k pixels from the south-west corner, so column k sits at
        /// fx = k and flipped row k sits at fy = k + 1.
        /// </summary>
        void Sample(double fx, double fy, ref double r, ref double g, ref double b)
        {
            fy -= 1;
            int x0 = (int)Math.Floor(fx), y0 = (int)Math.Floor(fy);
            double ax = fx - x0, ay = fy - y0;
            int xa = ((x0 % Width) + Width) % Width, xb = (xa + 1) % Width;
            int ya = Math.Max(0, Math.Min(Height - 1, y0)), yb = Math.Max(0, Math.Min(Height - 1, y0 + 1));
            int p00 = (ya * Width + xa) * 3, p10 = (ya * Width + xb) * 3, p01 = (yb * Width + xa) * 3, p11 = (yb * Width + xb) * 3;
            double w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
            r += Rgb[p00] * w00 + Rgb[p10] * w10 + Rgb[p01] * w01 + Rgb[p11] * w11;
            g += Rgb[p00 + 1] * w00 + Rgb[p10 + 1] * w10 + Rgb[p01 + 1] * w01 + Rgb[p11 + 1] * w11;
            b += Rgb[p00 + 2] * w00 + Rgb[p10 + 2] * w10 + Rgb[p01 + 2] * w01 + Rgb[p11 + 2] * w11;
        }
    }
}
