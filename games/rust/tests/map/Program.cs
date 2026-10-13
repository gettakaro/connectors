// Runs the connector's world-map region (lifted out of mod/TakaroConnector.cs by run.sh)
// against the tile layout Takaro's player map draws.
using System;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

internal static class Program
{
    private static int _checks;
    private static int _failures;

    private static void Check(bool ok, string what)
    {
        _checks++;
        if (ok) return;
        _failures++;
        Console.WriteLine("  FAIL: " + what);
    }

    private const byte BgR = 10, BgG = 20, BgB = 30;

    // A 1000 m world drawn at one pixel per metre, south-west corner at (-500, -500):
    // north-east red, north-west green, south-east blue, south-west white.
    private static Map.MapRaster Quadrants()
    {
        const int n = 1000;
        var rgb = new byte[n * n * 3];
        for (var py = 0; py < n; py++)
        {
            for (var px = 0; px < n; px++)
            {
                var east = px >= n / 2;
                var north = py >= n / 2;
                byte r, g, b;
                if (north && east) { r = 255; g = 0; b = 0; }
                else if (north) { r = 0; g = 255; b = 0; }
                else if (east) { r = 0; g = 0; b = 255; }
                else { r = 255; g = 255; b = 255; }
                var o = (py * n + px) * 3;
                rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = b;
            }
        }
        return new Map.MapRaster(rgb, n, n, 1000f, -500f, -500f, 1f, BgR, BgG, BgB, "test");
    }

    // Same world, every pixel encodes its own raster column (R) and row (G).
    private static Map.MapRaster Gradient()
    {
        const int n = 1000;
        var rgb = new byte[n * n * 3];
        for (var py = 0; py < n; py++)
            for (var px = 0; px < n; px++)
            {
                var o = (py * n + px) * 3;
                rgb[o] = (byte)(px % 256); rgb[o + 1] = (byte)(py % 256); rgb[o + 2] = 7;
            }
        return new Map.MapRaster(rgb, n, n, 1000f, -500f, -500f, 1f, BgR, BgG, BgB, "gradient");
    }

    private static (byte R, byte G, byte B) Pixel(byte[] tile, int column, int rowFromSouth)
    {
        var o = (rowFromSouth * Map.MapTilePixels + column) * 3;
        return (tile[o], tile[o + 1], tile[o + 2]);
    }

    private static bool Uniform(byte[] tile, byte r, byte g, byte b)
    {
        for (var i = 0; i < tile.Length; i += 3)
            if (tile[i] != r || tile[i + 1] != g || tile[i + 2] != b) return false;
        return true;
    }

    private static int Main()
    {
        Geometry();
        Info();
        Args();
        Tiles();
        WorldKey();
        Console.WriteLine("checks: " + _checks + ", failures: " + _failures);
        return _failures == 0 ? 0 : 1;
    }

    private static void Geometry()
    {
        Check(Map.MapTileWorldSize(4) == 128, "geometry: a zoom-4 tile spans 128 m (one metre per pixel)");
        Check(Map.MapTileWorldSize(0) == 2048, "geometry: a zoom-0 tile spans 2048 m");
        Check(Map.MapRenderScale(1000f) == 1f, "scale: a 1000 m world renders at one pixel per metre");
        Check(Math.Abs(Map.MapRenderScale(4500f) - 2048f / 4500f) < 1e-6, "scale: a 4500 m world fits 2048 px");
        Check(Map.MapRenderScale(20000f) == 0.25f, "scale: huge worlds stop at a quarter pixel per metre");
        Check(Map.MapRenderScale(0f) == 1f, "scale: an unknown size does not divide by zero");

        // 1000 m world + 512 m ocean pad = edge at +-1012 m.
        Check(Map.MapTileInRange(1000f, 4, 0, 0), "range: the tile north-east of the origin");
        Check(Map.MapTileInRange(1000f, 4, -1, -1), "range: the tile south-west of the origin");
        Check(Map.MapTileInRange(1000f, 4, 7, 7), "range: 896..1024 m overlaps the ocean pad");
        Check(Map.MapTileInRange(1000f, 4, -8, -8), "range: -1024..-896 m overlaps the ocean pad");
        Check(!Map.MapTileInRange(1000f, 4, 8, 0), "range: 1024 m and beyond is outside");
        Check(!Map.MapTileInRange(1000f, 4, -9, 0), "range: below -1024 m is outside");
        Check(!Map.MapTileInRange(1000f, 4, 0, 8), "range: far north is outside");
        Check(Map.MapTileInRange(1000f, 0, 0, 0) && Map.MapTileInRange(1000f, 0, -1, -1), "range: zoom 0 has the four tiles around the origin");
        Check(!Map.MapTileInRange(1000f, 0, 1, 0), "range: zoom 0 has nothing at 2048 m");
        Check(!Map.MapTileInRange(1000f, 5, 0, 0) && !Map.MapTileInRange(1000f, -1, 0, 0), "range: zoom outside 0..4 is refused");
        Check(!Map.MapTileInRange(0f, 4, 0, 0), "range: an unknown world size has no tiles");
    }

    private static void Info()
    {
        var info = Map.MapInfo(true, 1000f);
        Check(info.Value<bool>("enabled"), "info: enabled when the raster is ready");
        Check(info.Value<int>("mapBlockSize") == 128, "info: mapBlockSize is one zoom-4 tile in metres");
        Check(info.Value<int>("maxZoom") == 4, "info: maxZoom 4");
        Check(info.Value<int>("mapSizeX") == 1000 && info.Value<int>("mapSizeZ") == 1000, "info: X and Z are the world size");
        // The player map treats the smallest of Y/Z as vertical; Y must not read as smaller.
        Check(info.Value<int>("mapSizeY") >= info.Value<int>("mapSizeZ"), "info: Y reads as the up axis");
        Check(!Map.MapInfo(false, 0f).Value<bool>("enabled"), "info: disabled before the world loads");
        Check(Map.MapInfo(false, 0f).Value<int>("mapSizeX") == 0, "info: no size before the world loads");
    }

    private static void Args()
    {
        int x, y, z;
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":1,\"y\":-2,\"z\":3}"), out x, out y, out z) == null && x == 1 && y == -2 && z == 3,
            "args: integers");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":\"-4\",\"y\":\"5\",\"z\":\"0\"}"), out x, out y, out z) == null && x == -4 && y == 5 && z == 0,
            "args: numeric strings");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":2,\"y\":2,\"zoom\":4}"), out x, out y, out z) == null && z == 4,
            "args: zoom instead of z");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":2,\"y\":2,\"z\":null,\"zoom\":1}"), out x, out y, out z) == null && z == 1,
            "args: explicit null z falls back to zoom");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":2.0,\"y\":3,\"z\":4}"), out x, out y, out z) == null && x == 2,
            "args: a whole float is accepted");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":1.5,\"y\":3,\"z\":4}"), out x, out y, out z) != null, "args: a fraction is refused");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":1,\"z\":4}"), out x, out y, out z) != null, "args: a missing y is refused");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":null,\"y\":1,\"z\":4}"), out x, out y, out z) != null, "args: a null x is refused");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":\"a\",\"y\":1,\"z\":4}"), out x, out y, out z) != null, "args: a word is refused");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":1,\"y\":1,\"z\":5}"), out x, out y, out z) != null, "args: zoom 5 is refused");
        Check(Map.ParseMapTileArgs(JObject.Parse("{\"x\":1,\"y\":1,\"z\":-1}"), out x, out y, out z) != null, "args: a negative zoom is refused");
        Check(Map.ParseMapTileArgs(new JObject(), out x, out y, out z) != null, "args: empty args are refused");
        Check(Map.ParseMapTileArgs(null, out x, out y, out z) != null, "args: no args are refused");
    }

    private static void Tiles()
    {
        var quad = Quadrants();
        Check(Uniform(Map.RenderMapTileRgb(quad, 4, 0, 0), 255, 0, 0), "tiles: tile (0,0) is north-east of the origin");
        Check(Uniform(Map.RenderMapTileRgb(quad, 4, -1, 0), 0, 255, 0), "tiles: tile (-1,0) is north-west (x grows east)");
        Check(Uniform(Map.RenderMapTileRgb(quad, 4, 0, -1), 0, 0, 255), "tiles: tile (0,-1) is south-east (y grows north)");
        Check(Uniform(Map.RenderMapTileRgb(quad, 4, -1, -1), 255, 255, 255), "tiles: tile (-1,-1) is south-west");
        Check(Uniform(Map.RenderMapTileRgb(quad, 4, 7, 7), BgR, BgG, BgB), "tiles: past the world edge is open sea");

        // Tile (3,0) spans x 384..512: column 115 is x 499.5 (land), column 116 is x 500.5 (sea).
        var edge = Map.RenderMapTileRgb(quad, 4, 3, 0);
        Check(Pixel(edge, 115, 0) == (255, 0, 0), "tiles: last world column is land");
        Check(Pixel(edge, 116, 0) == (BgR, BgG, BgB), "tiles: first column past the edge is sea");

        // Row 0 of a tile is its southern edge, column 0 its western edge.
        var gradient = Gradient();
        var tile = Map.RenderMapTileRgb(gradient, 4, 0, 0);
        Check(Pixel(tile, 0, 0) == ((byte)(500 % 256), (byte)(500 % 256), 7), "orientation: pixel (0,0) is the tile's south-west metre");
        Check(Pixel(tile, 10, 0).R == (byte)(510 % 256), "orientation: columns grow east");
        Check(Pixel(tile, 0, 20).G == (byte)(520 % 256), "orientation: rows grow north");
        var south = Map.RenderMapTileRgb(gradient, 4, -2, -3);
        Check(Pixel(south, 5, 6) == ((byte)((500 - 256 + 5) % 256), (byte)((500 - 384 + 6) % 256), 7),
            "orientation: negative tiles land at -256 m east, -384 m north");

        // Zoomed out, a pixel averages its area: zoom 0 is 16 m per pixel, 4x4 samples.
        var far = Map.RenderMapTileRgb(quad, 0, 0, 0);
        Check(Pixel(far, 0, 0) == (255, 0, 0), "downsample: inside the quadrant stays pure");
        var mixed = Pixel(far, 31, 0); // x 496..512, samples at 498, 502, 506, 510
        Check(mixed.R == (byte)((255 + 3 * BgR + 2) / 4) && mixed.G == (byte)((3 * BgG + 2) / 4),
            "downsample: the edge pixel blends one land sample with three sea samples");
        var southWest = Map.RenderMapTileRgb(quad, 0, -1, -1);
        Check(Pixel(southWest, 127, 127) == (255, 255, 255) && Pixel(southWest, 0, 0) == (BgR, BgG, BgB),
            "downsample: the south-west zoom-0 tile has land at its north-east corner, sea at its south-west");

        Check(Map.MapRasterIndex(quad, -500.0, -500.0) == 0, "raster: the south-west corner is index 0");
        Check(Map.MapRasterIndex(quad, 499.9, 499.9) == (999 * 1000 + 999) * 3, "raster: the north-east corner is the last pixel");
        Check(Map.MapRasterIndex(quad, 500.0, 0) == -1 && Map.MapRasterIndex(quad, 0, -500.1) == -1, "raster: outside the world is -1");

        // Half a pixel per metre: a 2000 m world in a 1000 px raster.
        var half = new Map.MapRaster(new byte[1000 * 1000 * 3], 1000, 1000, 2000f, -1000f, -1000f, 0.5f, 0, 0, 0, "half");
        Check(Map.MapRasterIndex(half, 999.0, 999.0) == (999 * 1000 + 999) * 3, "raster: scale below one pixel per metre");

        var threw = false;
        try { new Map.MapRaster(new byte[10], 2, 2, 1000f, 0, 0, 1f, 0, 0, 0, "bad"); }
        catch (ArgumentException) { threw = true; }
        Check(threw, "raster: a buffer of the wrong size is refused");
    }

    private static void WorldKey()
    {
        var a = Map.MapWorldKey(1000, 12345, "proceduralmap.1000.12345.289.map", "abc");
        Check(a == Map.MapWorldKey(1000, 12345, "proceduralmap.1000.12345.289.map", "abc"), "key: stable for the same world");
        Check(a != Map.MapWorldKey(1000, 12346, "proceduralmap.1000.12346.289.map", "abc"), "key: a new seed is a new map");
        Check(a != Map.MapWorldKey(3000, 12345, "proceduralmap.3000.12345.289.map", "abc"), "key: a new size is a new map");
        Check(a != Map.MapWorldKey(1000, 12345, "proceduralmap.1000.12345.290.map", "abc"), "key: a game protocol bump is a new map");
        Check(a != Map.MapWorldKey(1000, 12345, "proceduralmap.1000.12345.289.map", "def"), "key: a new checksum (custom map) is a new map");
        Check(Map.MapWorldKey(0, 0, null, null).Length > 0, "key: missing names still give a key");
        Check(Regex.IsMatch(a, "^[0-9a-z_]+$"), "key: safe as a file name");
    }
}
