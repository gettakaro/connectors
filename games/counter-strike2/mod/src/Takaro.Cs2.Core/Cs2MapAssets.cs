using System.Globalization;
using System.Text.RegularExpressions;
using StbImageSharp;

namespace Takaro.Cs2.Core;

public sealed class Cs2MapAssets
{
    private const int TilePixels = 128;
    private const int MaxZoom = 4;
    private readonly string directory;
    private readonly Dictionary<string, MapData> loaded = new(StringComparer.Ordinal);
    private readonly object gate = new();

    public Cs2MapAssets(string directory) => this.directory = directory;

    public TakaroActionResult GetInfo(string map)
    {
        if (!TryLoad(map, out var data, out _))
            return TakaroActionResult.Ok(new { enabled = false, mapBlockSize = 0, maxZoom = 0, mapSizeX = 0, mapSizeY = 0, mapSizeZ = 0 });
        var extent = (int)Math.Ceiling(Math.Max(data.Width, data.Height) * data.Scale);
        return TakaroActionResult.Ok(new { enabled = true, mapBlockSize = TilePixels, maxZoom = MaxZoom,
            mapSizeX = extent, mapSizeY = extent, mapSizeZ = extent });
    }

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
        var worldSize = TilePixels * (1 << (MaxZoom - zoom));
        var left = (long)x * worldSize;
        var bottom = (long)y * worldSize;
        if (left >= data.MaxX || left + worldSize <= data.MinX ||
            bottom >= data.MaxY || bottom + worldSize <= data.MinY)
            return TakaroActionResult.Fail("tile_out_of_range", "The requested tile does not overlap this map.");
        lock (data.Tiles)
            if (data.Tiles.TryGetValue((x, y, zoom), out var cached))
                return TakaroActionResult.Ok(cached);

        var rgba = new byte[TilePixels * TilePixels * 4];
        for (var row = 0; row < TilePixels; row++)
        for (var column = 0; column < TilePixels; column++)
        {
            var worldX = left + (column + .5) * worldSize / TilePixels;
            var worldY = bottom + worldSize - (row + .5) * worldSize / TilePixels;
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
        return TakaroActionResult.Ok(encoded);
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
                var locations = ReadSpawnLocations(File.ReadAllText(entities));
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

    private static object[] ReadSpawnLocations(string text)
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
                locations.Add(new { code, name, position = new {
                    x = representative.X,
                    y = representative.Y,
                    z = representative.Z } });
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
