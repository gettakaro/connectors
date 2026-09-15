using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace Takaro.Services
{
    /// <summary>
    /// Pure helpers over the 7 Days to Die web-map tile cache.
    ///
    /// The dedicated server only writes map tiles when the web dashboard is
    /// enabled (<c>WebDashboardEnabled</c> in serverconfig.xml). The tiles live
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
    }
}
