using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace Takaro.Services
{
    /// <summary>
    /// Pure helpers over the 7 Days to Die web-map tile cache.
    ///
    /// The dedicated server only writes map tiles when map rendering is
    /// enabled (<c>EnableMapRendering</c> in serverconfig.xml). The tiles live
    /// under <c>&lt;save&gt;/map/&lt;zoom&gt;/&lt;x&gt;/&lt;y&gt;.png</c>, one
    /// directory level per zoom. Nothing here touches game APIs, so it is fully
    /// covered by the contract harness.
    /// </summary>
    public static class MapCatalog
    {
        /// <summary>Blocks covered by one tile edge at the highest zoom level.</summary>
        public const int TileBlockSize = 16;

        /// <summary>Zoom reported when the cache exists but no zoom level could be read.</summary>
        public const int FallbackMaxZoom = 4;

        /// <summary>
        /// Inspects a tile-cache root. Returns whether usable tiles exist and the
        /// highest zoom level present.
        /// </summary>
        public static bool TryInspect(string mapRoot, out int maxZoom)
        {
            maxZoom = 0;

            if (string.IsNullOrEmpty(mapRoot))
                return false;

            string[] zoomDirs;
            try
            {
                if (!Directory.Exists(mapRoot))
                    return false;
                zoomDirs = Directory.GetDirectories(mapRoot);
            }
            catch (Exception)
            {
                // Unreadable cache is indistinguishable from "no map" for Takaro.
                return false;
            }

            bool found = false;
            foreach (string dir in zoomDirs)
            {
                string name = Path.GetFileName(dir);
                int zoom;
                if (!int.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out zoom))
                    continue;

                found = true;
                if (zoom > maxZoom)
                    maxZoom = zoom;
            }

            if (!found)
                return false;

            if (maxZoom <= 0)
                maxZoom = FallbackMaxZoom;

            return true;
        }

        /// <summary>
        /// Builds a payload that satisfies Takaro's MapInfoDTO. Every property is
        /// always present and correctly typed — in particular <c>enabled</c> is a
        /// real boolean, never null or missing.
        /// </summary>
        public static Dictionary<string, object> BuildMapInfo(
            string mapRoot,
            int mapSizeX,
            int mapSizeY,
            int mapSizeZ
        )
        {
            int maxZoom;
            bool enabled = TryInspect(mapRoot, out maxZoom);
            if (!enabled)
                maxZoom = FallbackMaxZoom;

            return new Dictionary<string, object>
            {
                { "enabled", enabled },
                { "mapBlockSize", TileBlockSize },
                { "maxZoom", maxZoom },
                { "mapSizeX", Math.Max(0, mapSizeX) },
                { "mapSizeY", Math.Max(0, mapSizeY) },
                { "mapSizeZ", Math.Max(0, mapSizeZ) },
            };
        }

        /// <summary>
        /// Resolves the on-disk path of a single tile. Returns null when the
        /// cache root is unset, so callers report a clear error instead of
        /// throwing.
        /// </summary>
        public static string ResolveTilePath(string mapRoot, int zoom, int x, int y)
        {
            if (string.IsNullOrEmpty(mapRoot))
                return null;

            return Path.Combine(
                mapRoot,
                zoom.ToString(CultureInfo.InvariantCulture),
                x.ToString(CultureInfo.InvariantCulture),
                y.ToString(CultureInfo.InvariantCulture) + ".png"
            );
        }

        /// <summary>
        /// Reads a tile and returns it base64-encoded, or null when the tile has
        /// not been rendered yet.
        /// </summary>
        public static string TryReadTileBase64(string mapRoot, int zoom, int x, int y)
        {
            string path = ResolveTilePath(mapRoot, zoom, x, y);
            if (string.IsNullOrEmpty(path))
                return null;

            try
            {
                if (!File.Exists(path))
                    return null;
                return Convert.ToBase64String(File.ReadAllBytes(path));
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>Pixel edge of a game tile when mapinfo.json is missing (the game's default).</summary>
        public const int DefaultTilePixels = 128;

        /// <summary>Highest game zoom when mapinfo.json is missing (the game's default of 5 levels).</summary>
        public const int DefaultGameMaxZoom = 4;

        /// <summary>
        /// Reads the renderer's layout from <c>mapinfo.json</c>
        /// (<c>{"blockSize":128,"maxZoom":4}</c>, written by the game's MapRenderer).
        /// Falls back to the game defaults when the file is missing or unreadable.
        /// </summary>
        public static void ReadTileLayout(string mapRoot, out int tilePixels, out int gameMaxZoom)
        {
            tilePixels = DefaultTilePixels;
            gameMaxZoom = DefaultGameMaxZoom;
            if (string.IsNullOrEmpty(mapRoot))
                return;

            try
            {
                string path = Path.Combine(mapRoot, "mapinfo.json");
                if (!File.Exists(path))
                    return;
                var info = Newtonsoft.Json.Linq.JObject.Parse(File.ReadAllText(path));
                var blockSize = info["blockSize"];
                var maxZoom = info["maxZoom"];
                if (blockSize != null && blockSize.Type == Newtonsoft.Json.Linq.JTokenType.Integer)
                    tilePixels = (int)blockSize;
                if (maxZoom != null && maxZoom.Type == Newtonsoft.Json.Linq.JTokenType.Integer)
                    gameMaxZoom = (int)maxZoom;
            }
            catch (Exception)
            {
                // A damaged mapinfo.json keeps the defaults the game itself would use.
            }

            if (tilePixels < 1)
                tilePixels = DefaultTilePixels;
            if (gameMaxZoom < 0)
                gameMaxZoom = DefaultGameMaxZoom;
        }
    }

    /// <summary>
    /// The protocol v1 <c>map</c> frame over the 7D2D tile cache. The game cuts
    /// tiles at signed indices around the world origin: at game zoom
    /// <c>g</c> tile (x, y) covers blocks <c>[x*s, (x+1)*s)</c> along x and
    /// <c>[y*s, (y+1)*s)</c> along z (north), with <c>s = tilePixels *
    /// 2^(gameMaxZoom - g)</c>; the PNG is north-up. Takaro wants the standard
    /// XYZ pyramid over a square frame (zoom <c>z</c> = 2^z x 2^z tiles,
    /// first row at the top). The frame here is the square <c>[-half, half]²</c>
    /// with <c>half</c> a power-of-two multiple of the game's zoom-0 tile, so
    /// every Takaro tile is exactly one game tile: the zoom shifts by
    /// <see cref="ZoomOffset"/>, x by <c>2^(z-1)</c>, and y is flipped.
    /// </summary>
    public sealed class MapFrame
    {
        /// <summary>The only Dimension; positions carry none, so Takaro uses it.</summary>
        public const string DimensionId = "world";

        public int TilePixels { get; private set; }
        public int GameMaxZoom { get; private set; }

        /// <summary>Takaro zoom of game zoom 0; at least 1, so the frame centre is a tile edge.</summary>
        public int ZoomOffset { get; private set; }

        /// <summary>Half the frame edge in blocks.</summary>
        public long HalfSize { get; private set; }

        public int MinZoom => ZoomOffset;
        public int MaxZoom => ZoomOffset + GameMaxZoom;

        /// <param name="worldHalfExtent">Largest |x| or |z| of the world, in blocks; 0 when unknown.</param>
        public static MapFrame ForWorld(int tilePixels, int gameMaxZoom, int worldHalfExtent)
        {
            long gameZoomZeroEdge = (long)tilePixels << gameMaxZoom;
            long half = gameZoomZeroEdge;
            int offset = 1;
            while (half < worldHalfExtent && offset < 20 - gameMaxZoom)
            {
                half *= 2;
                offset++;
            }

            return new MapFrame
            {
                TilePixels = tilePixels,
                GameMaxZoom = gameMaxZoom,
                ZoomOffset = offset,
                HalfSize = half,
            };
        }

        /// <summary>
        /// Translates a Takaro tile request to the game's tile index. False when
        /// the tile lies outside the declared pyramid.
        /// </summary>
        public bool TryToGameTile(
            int zoom,
            int x,
            int y,
            out int gameZoom,
            out int gameX,
            out int gameY
        )
        {
            gameZoom = 0;
            gameX = 0;
            gameY = 0;
            if (zoom < MinZoom || zoom > MaxZoom)
                return false;

            long tilesPerSide = 1L << zoom;
            if (x < 0 || y < 0 || x >= tilesPerSide || y >= tilesPerSide)
                return false;

            long centre = tilesPerSide / 2;
            gameZoom = zoom - ZoomOffset;
            gameX = (int)(x - centre);
            gameY = (int)(centre - 1 - y);
            return true;
        }

        /// <summary>The <c>map</c> Capability: one tiled Dimension, +x right, +z (north) up.</summary>
        public Dictionary<string, object> BuildCapability(string worldName)
        {
            string name = string.IsNullOrWhiteSpace(worldName) ? "World" : worldName.Trim();
            if (name.Length > 64)
                name = name.Substring(0, 64);

            return new Dictionary<string, object>
            {
                { "name", "map" },
                {
                    "parameters",
                    new Dictionary<string, object>
                    {
                        {
                            "dimensions",
                            new List<Dictionary<string, object>>
                            {
                                new Dictionary<string, object>
                                {
                                    { "id", DimensionId },
                                    { "name", name },
                                    {
                                        "axes",
                                        new Dictionary<string, object>
                                        {
                                            {
                                                "horizontal",
                                                new Dictionary<string, object>
                                                {
                                                    { "field", "x" },
                                                    { "direction", "right" },
                                                }
                                            },
                                            {
                                                "vertical",
                                                new Dictionary<string, object>
                                                {
                                                    { "field", "z" },
                                                    { "direction", "up" },
                                                }
                                            },
                                        }
                                    },
                                    {
                                        "bounds",
                                        new Dictionary<string, object>
                                        {
                                            { "horizontal", Range() },
                                            { "vertical", Range() },
                                        }
                                    },
                                    {
                                        "source",
                                        new Dictionary<string, object>
                                        {
                                            { "type", "tiled" },
                                            { "tileSize", TilePixels },
                                            { "minZoom", MinZoom },
                                            { "maxZoom", MaxZoom },
                                        }
                                    },
                                },
                            }
                        },
                    }
                },
            };
        }

        private Dictionary<string, object> Range()
        {
            return new Dictionary<string, object> { { "min", -HalfSize }, { "max", HalfSize } };
        }
    }
}
