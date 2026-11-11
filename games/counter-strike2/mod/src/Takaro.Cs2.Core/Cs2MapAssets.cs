using System.Globalization;
using System.Text.RegularExpressions;
using StbImageSharp;

namespace Takaro.Cs2.Core;

public sealed class Cs2MapAssets
{
    private const int TilePixels = 256;
    private const int MaxZoom = 3;
    private const int SpawnRadius = 256;
    private readonly string directory;
    private readonly Dictionary<string, MapData> loaded = new(StringComparer.Ordinal);
    private readonly object gate = new();

    private readonly Action<string> log;

    public Cs2MapAssets(string directory, Action<string>? log = null)
    {
        this.directory = directory;
        this.log = log ?? (_ => { });
    }

    /// <summary>The protocol v1 map Dimension for a prepared map, or null when its assets are missing.</summary>
    public object? DescribeDimension(string map)
    {
        if (!TryLoad(map, out var data, out var reason))
        {
            log($"Map is not available to Takaro: {reason}");
            return null;
        }
        return new
        {
            id = map,
            name = map,
            axes = new
            {
                horizontal = new { field = "x", direction = "right" },
                vertical = new { field = "y", direction = "up" }
            },
            bounds = new
            {
                horizontal = new { min = data.MinX, max = data.MaxX },
                vertical = new { min = data.MinY, max = data.MaxY }
            },
            source = new { type = "tiled", tileSize = TilePixels, minZoom = 0, maxZoom = MaxZoom }
        };
    }

    public bool HasLocations(string map) => TryLoad(map, out var data, out _) && data.Locations.Length > 0;

    public TakaroActionResult ListLocations(string map)
    {
        if (!TryLoad(map, out var data, out var reason))
            return TakaroActionResult.Fail("map_unavailable", reason);
        if (data.Locations.Length == 0)
            return TakaroActionResult.Fail("locations_unavailable", $"Map '{map}' has no validated spawn positions.");
        return TakaroActionResult.Ok(data.Locations);
    }

    public TakaroActionResult GetTile(string map, int x, int y, int zoom)
    {
        if (zoom is < 0 or > MaxZoom)
            return TakaroActionResult.Fail("invalid_args", $"Map zoom must be between 0 and {MaxZoom}.");
        if (!TryLoad(map, out var data, out var reason))
            return TakaroActionResult.Fail("map_unavailable", reason);
        if (x < 0 || y < 0 || x >= 1 << zoom || y >= 1 << zoom)
            return TakaroActionResult.Fail("tile_out_of_range", "The requested tile is outside this zoom level.");
        var frame = Math.Max(data.MaxX - data.MinX, data.MaxY - data.MinY);
        var worldSize = frame / (1 << zoom);
        var left = data.MinX + x * worldSize;
        var top = data.MaxY - y * worldSize;
        lock (data.Tiles)
            if (data.Tiles.TryGetValue((x, y, zoom), out var cached))
                return TakaroActionResult.Ok(new { mimeType = "image/png", data = cached });

        var rgba = new byte[TilePixels * TilePixels * 4];
        for (var row = 0; row < TilePixels; row++)
        for (var column = 0; column < TilePixels; column++)
        {
            var worldX = left + (column + .5) * worldSize / TilePixels;
            var worldY = top - (row + .5) * worldSize / TilePixels;
            var px = (int)Math.Floor((worldX - data.MinX) / data.Scale);
            var py = (int)Math.Floor((data.MaxY - worldY) / data.Scale);
            if (px < 0 || px >= data.Width || py < 0 || py >= data.Height) continue;
            Buffer.BlockCopy(data.Rgba, (py * data.Width + px) * 4, rgba,
                (row * TilePixels + column) * 4, 4);
        }

        using var stream = new MemoryStream();
        new StbImageWriteSharp.ImageWriter().WritePng(rgba, TilePixels, TilePixels,
            StbImageWriteSharp.ColorComponents.RedGreenBlueAlpha, stream);
        var encoded = Convert.ToBase64String(stream.ToArray());
        lock (data.Tiles)
        {
            if (data.Tiles.Count >= 256)
                data.Tiles.Remove(data.TileOrder.Dequeue());
            data.Tiles[(x, y, zoom)] = encoded;
            data.TileOrder.Enqueue((x, y, zoom));
        }
        return TakaroActionResult.Ok(new { mimeType = "image/png", data = encoded });
    }

    private bool TryLoad(string map, out MapData data, out string reason)
    {
        data = null!;
        reason = "";
        if (!Regex.IsMatch(map, @"^[a-zA-Z0-9_]+$"))
        {
            reason = "Invalid map name.";
            return false;
        }
        lock (gate)
        {
            if (loaded.TryGetValue(map, out data!)) return true;
            var png = Path.Combine(directory, map + ".png");
            var overview = Path.Combine(directory, map + ".txt");
            var entities = Path.Combine(directory, map + ".vents");
            if (!File.Exists(png) || !File.Exists(overview) || !File.Exists(entities))
            {
                reason = $"Map '{map}' has no prepared server-owned radar, overview, and entity assets.";
                return false;
            }
            try
            {
                var source = ImageResult.FromMemory(File.ReadAllBytes(png), StbImageSharp.ColorComponents.RedGreenBlueAlpha);
                if (source.Width is < 64 or > 4096 || source.Height is < 64 or > 4096)
                    throw new InvalidDataException("Radar dimensions are outside supported bounds.");
                var text = File.ReadAllText(overview);
                var posX = ReadOverviewNumber(text, "pos_x");
                var posY = ReadOverviewNumber(text, "pos_y");
                var scale = ReadOverviewNumber(text, "scale");
                if (scale is <= 0 or > 100) throw new InvalidDataException("Radar scale is invalid.");
                var locations = ReadSpawnLocations(File.ReadAllText(entities), map);
                data = new MapData(source.Width, source.Height, source.Data, posX, posY, scale, locations);
                loaded[map] = data;
                return true;
            }
            catch (Exception ex)
            {
                reason = $"Map '{map}' assets could not be decoded: {ex.Message}";
                return false;
            }
        }
    }

    private static double ReadOverviewNumber(string text, string key)
    {
        var match = Regex.Match(text, $@"(?m)^\s*""{Regex.Escape(key)}""\s*""([^""]+)""");
        if (!match.Success || !double.TryParse(match.Groups[1].Value, NumberStyles.Float,
                CultureInfo.InvariantCulture, out var value) || !double.IsFinite(value))
            throw new InvalidDataException($"Overview has no valid {key}.");
        return value;
    }

    private static object[] ReadSpawnLocations(string text, string map)
    {
        var locations = new List<object>();
        foreach (var (entity, code, name) in new[] {
            ("info_player_terrorist", "t_spawn", "T Spawn"),
            ("info_player_counterterrorist", "ct_spawn", "CT Spawn")
        })
        {
            var points = new List<(double X, double Y, double Z)>();
            foreach (var block in Regex.Split(text, @"(?m)^====\d+====\s*$"))
            {
                if (!Regex.IsMatch(block, $@"(?m)^classname\s+""{entity}""\s*$")) continue;
                var match = Regex.Match(block, @"(?m)^origin\s+\[\s*([^,]+),\s*([^,]+),\s*([^\]]+)\]");
                if (!match.Success) continue;
                if (!double.TryParse(match.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var x) ||
                    !double.TryParse(match.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var y) ||
                    !double.TryParse(match.Groups[3].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var z) ||
                    !double.IsFinite(x) || !double.IsFinite(y) || !double.IsFinite(z)) continue;
                points.Add((x, y, z));
            }
            if (points.Count > 0)
            {
                var centerX = points.Average(point => point.X);
                var centerY = points.Average(point => point.Y);
                var centerZ = points.Average(point => point.Z);
                var representative = points.MinBy(point =>
                    Math.Pow(point.X - centerX, 2) + Math.Pow(point.Y - centerY, 2) + Math.Pow(point.Z - centerZ, 2));
                locations.Add(new { code, name, category = "spawn", radius = SpawnRadius, position = new {
                    x = representative.X,
                    y = representative.Y,
                    z = representative.Z,
                    dimension = map } });
            }
        }
        return locations.ToArray();
    }

    private sealed record MapData(int Width, int Height, byte[] Rgba, double MinX, double MaxY,
        double Scale, object[] Locations)
    {
        public double MaxX => MinX + Width * Scale;
        public double MinY => MaxY - Height * Scale;
        public Dictionary<(int X, int Y, int Zoom), string> Tiles { get; } = new();
        public Queue<(int X, int Y, int Zoom)> TileOrder { get; } = new();
    }
}
