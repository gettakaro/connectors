using System.IO.Compression;
using System.Text.Json.Nodes;

namespace Takaro.Valheim.Core;

/// <summary>
/// Valheim biomes as stored in the world-map raster. The values are the connector's own
/// (one byte per cell), not <c>Heightmap.Biome</c> flags, so Core stays free of game types.
/// </summary>
public enum WorldMapBiome : byte
{
    Unknown = 0,
    Meadows = 1,
    Swamp = 2,
    Mountain = 3,
    BlackForest = 4,
    Plains = 5,
    AshLands = 6,
    DeepNorth = 7,
    Ocean = 8,
    Mistlands = 9
}

/// <summary>One world sample: what the game's world generator says about a point.</summary>
public readonly record struct WorldMapSample(WorldMapBiome Biome, float Height, bool Forest);

/// <summary>
/// Tile layout served through <c>getMapInfo</c>/<c>getMapTile</c>.
///
/// It follows the 7 Days to Die web-map convention that the Takaro player map already
/// renders: signed tile indices centred on world origin, 128 px tiles, tile y grows north
/// (+Z), and at <see cref="MaxZoom"/> one pixel is one world unit (metre). At zoom z one tile
/// therefore spans <c>128 * 2^(MaxZoom - z)</c> metres.
/// </summary>
public static class WorldMapGeometry
{
    public const int TilePixels = 128;
    public const int MaxZoom = 4;

    /// <summary>Valheim's playable world ends at radius 10000 m; terrain stops at 10500 m.</summary>
    public const float WorldEdgeRadius = 10500f;

    /// <summary>Full horizontal extent (diameter) of the world in metres.</summary>
    public const int WorldExtent = 21000;

    /// <summary>Water level used by the game's own minimap.</summary>
    public const float WaterLevel = 30f;

    public static double TileWorldSize(int zoom) => TilePixels * Math.Pow(2, MaxZoom - zoom);

    public static bool IsValidZoom(int zoom) => zoom >= 0 && zoom <= MaxZoom;

    /// <summary>True when the tile overlaps the world square [-edge, edge]².</summary>
    public static bool IntersectsWorld(int zoom, int x, int y)
    {
        if (!IsValidZoom(zoom))
        {
            return false;
        }

        var size = TileWorldSize(zoom);
        var minX = x * size;
        var minZ = y * size;
        return minX < WorldEdgeRadius && minX + size > -WorldEdgeRadius
            && minZ < WorldEdgeRadius && minZ + size > -WorldEdgeRadius;
    }

    /// <summary>
    /// World X/Z at the centre of pixel (column, row) of tile (zoom, x, y). Row 0 is the
    /// northern (top) edge of the image.
    /// </summary>
    public static (double WorldX, double WorldZ) PixelCenter(int zoom, int x, int y, int column, int row)
    {
        var size = TileWorldSize(zoom);
        var metresPerPixel = size / TilePixels;
        return (x * size + (column + 0.5) * metresPerPixel,
            (y + 1) * size - (row + 0.5) * metresPerPixel);
    }

    /// <summary>
    /// Inverse of <see cref="PixelCenter"/>: the tile and pixel that contain a world point.
    /// </summary>
    public static (int TileX, int TileY, int Column, int Row) Locate(int zoom, double worldX, double worldZ)
    {
        var size = TileWorldSize(zoom);
        var tileX = (int)Math.Floor(worldX / size);
        var tileY = (int)Math.Floor(worldZ / size);
        var metresPerPixel = size / TilePixels;
        var column = (int)Math.Floor((worldX - tileX * size) / metresPerPixel);
        var row = (int)Math.Floor(((tileY + 1) * size - worldZ) / metresPerPixel);
        return (tileX, tileY, Math.Min(column, TilePixels - 1), Math.Min(row, TilePixels - 1));
    }

    /// <summary>
    /// Payload for Takaro's MapInfoDTO. <c>mapBlockSize</c> is the world size of one tile at
    /// <c>maxZoom</c>. Valheim's map is flat, so every <c>mapSize*</c> is the horizontal
    /// extent: Takaro's simulator reads X/Y as the horizontal axes and 7 Days to Die reads
    /// X/Z, and both readings stay correct this way.
    /// </summary>
    public static object CreateMapInfo(bool enabled) => new
    {
        enabled,
        mapBlockSize = TilePixels,
        maxZoom = MaxZoom,
        mapSizeX = WorldExtent,
        mapSizeY = WorldExtent,
        mapSizeZ = WorldExtent
    };
}

/// <summary>
/// Tile layout of the protocol version 1 <c>map</c> capability: one Dimension, the world square
/// [-edge, edge]² with +X to the right and +Z (north) up, cut into the standard XYZ pyramid
/// (zoom 0 is one tile over the whole frame, tile y grows down). At <see cref="MaxZoom"/> a
/// pixel is about 5 m, finer than the 10 m raster underneath.
/// </summary>
public static class WorldMapFrame
{
    /// <summary>Equals the <c>dimension</c> every Valheim position reports.</summary>
    public const string DimensionId = "valheim";
    public const int TileSize = 256;
    public const int MinZoom = 0;
    public const int MaxZoom = 4;

    public static double TileWorldSize(int zoom) => 2.0 * WorldMapGeometry.WorldEdgeRadius / (1 << zoom);

    public static bool IsValidTile(int zoom, int x, int y) =>
        zoom >= MinZoom && zoom <= MaxZoom && x >= 0 && y >= 0 && x < 1 << zoom && y < 1 << zoom;

    /// <summary>World X/Z at the centre of pixel (column, row); row 0 is the top (north) edge.</summary>
    public static (double WorldX, double WorldZ) PixelCenter(int zoom, int x, int y, int column, int row)
    {
        var size = TileWorldSize(zoom);
        var metresPerPixel = size / TileSize;
        return (-WorldMapGeometry.WorldEdgeRadius + x * size + (column + 0.5) * metresPerPixel,
            WorldMapGeometry.WorldEdgeRadius - y * size - (row + 0.5) * metresPerPixel);
    }

    public static JsonObject CreateCapability()
    {
        var edge = (double)WorldMapGeometry.WorldEdgeRadius;
        return new JsonObject
        {
            ["name"] = "map",
            ["parameters"] = new JsonObject
            {
                ["dimensions"] = new JsonArray
                {
                    new JsonObject
                    {
                        ["id"] = DimensionId,
                        ["name"] = "Valheim",
                        ["axes"] = new JsonObject
                        {
                            ["horizontal"] = new JsonObject { ["field"] = "x", ["direction"] = "right" },
                            ["vertical"] = new JsonObject { ["field"] = "z", ["direction"] = "up" }
                        },
                        ["bounds"] = new JsonObject
                        {
                            ["horizontal"] = new JsonObject { ["min"] = -edge, ["max"] = edge },
                            ["vertical"] = new JsonObject { ["min"] = -edge, ["max"] = edge }
                        },
                        ["source"] = new JsonObject
                        {
                            ["type"] = "tiled",
                            ["tileSize"] = TileSize,
                            ["minZoom"] = MinZoom,
                            ["maxZoom"] = MaxZoom
                        }
                    }
                }
            }
        };
    }
}

/// <summary>
/// A square, immutable-once-published raster of world samples covering [-edge, edge]².
/// Cell (row 0, column 0) is the south-west corner. Built on the Unity main thread, then
/// read from any thread.
/// </summary>
public sealed class WorldMapRaster
{
    private const uint Magic = 0x564D5254; // "TRMV"
    private const int FormatVersion = 1;
    private const int HeaderBytes = 4 + 4 + 4 + 4 + 8;

    private readonly byte[] cells;
    private readonly short[] heights;

    public WorldMapRaster(int size, long worldKey)
    {
        if (size < 2 || size > 8192)
        {
            throw new ArgumentOutOfRangeException(nameof(size), "Raster size must be between 2 and 8192.");
        }

        Size = size;
        WorldKey = worldKey;
        cells = new byte[size * size];
        heights = new short[size * size];
    }

    public int Size { get; }

    /// <summary>Identifies the world the raster was sampled from (seed and generator version).</summary>
    public long WorldKey { get; }

    public double CellSize => 2.0 * WorldMapGeometry.WorldEdgeRadius / Size;

    /// <summary>World X (column) or Z (row) at the centre of a raster index.</summary>
    public double CellCenter(int index) => -WorldMapGeometry.WorldEdgeRadius + (index + 0.5) * CellSize;

    public void Set(int row, int column, WorldMapSample sample)
    {
        var index = row * Size + column;
        cells[index] = (byte)((byte)sample.Biome | (sample.Forest ? 0x80 : 0));
        heights[index] = EncodeHeight(sample.Height);
    }

    public WorldMapBiome BiomeAt(int row, int column) => (WorldMapBiome)(cells[Index(row, column)] & 0x7F);

    public bool ForestAt(int row, int column) => (cells[Index(row, column)] & 0x80) != 0;

    public float HeightAt(int row, int column) => heights[Index(row, column)] / 10f;

    /// <summary>Nearest cell for a world point, clamped to the raster.</summary>
    public (int Row, int Column) CellOf(double worldX, double worldZ)
    {
        var column = (int)Math.Floor((worldX + WorldMapGeometry.WorldEdgeRadius) / CellSize);
        var row = (int)Math.Floor((worldZ + WorldMapGeometry.WorldEdgeRadius) / CellSize);
        return (Clamp(row), Clamp(column));
    }

    /// <summary>Bilinear height at a world point, clamped to the raster.</summary>
    public float SampleHeight(double worldX, double worldZ)
    {
        var fx = (worldX + WorldMapGeometry.WorldEdgeRadius) / CellSize - 0.5;
        var fz = (worldZ + WorldMapGeometry.WorldEdgeRadius) / CellSize - 0.5;
        var x0 = (int)Math.Floor(fx);
        var z0 = (int)Math.Floor(fz);
        var tx = (float)(fx - x0);
        var tz = (float)(fz - z0);
        var h00 = HeightAt(Clamp(z0), Clamp(x0));
        var h10 = HeightAt(Clamp(z0), Clamp(x0 + 1));
        var h01 = HeightAt(Clamp(z0 + 1), Clamp(x0));
        var h11 = HeightAt(Clamp(z0 + 1), Clamp(x0 + 1));
        var south = h00 + (h10 - h00) * tx;
        var north = h01 + (h11 - h01) * tx;
        return south + (north - south) * tz;
    }

    public byte[] Serialize()
    {
        var bytes = new byte[HeaderBytes + cells.Length + heights.Length * 2];
        using var stream = new MemoryStream(bytes);
        using var writer = new BinaryWriter(stream);
        writer.Write(Magic);
        writer.Write(FormatVersion);
        writer.Write(Size);
        writer.Write(0);
        writer.Write(WorldKey);
        writer.Write(cells);
        foreach (var height in heights)
        {
            writer.Write(height);
        }

        return bytes;
    }

    /// <summary>Reads a serialized raster; null when it is malformed or for another world.</summary>
    public static WorldMapRaster? TryDeserialize(byte[] bytes, int expectedSize, long expectedWorldKey)
    {
        try
        {
            using var reader = new BinaryReader(new MemoryStream(bytes));
            if (reader.ReadUInt32() != Magic || reader.ReadInt32() != FormatVersion)
            {
                return null;
            }

            var size = reader.ReadInt32();
            reader.ReadInt32();
            var worldKey = reader.ReadInt64();
            if (size != expectedSize || worldKey != expectedWorldKey
                || bytes.Length != HeaderBytes + size * size * 3)
            {
                return null;
            }

            var raster = new WorldMapRaster(size, worldKey);
            reader.Read(raster.cells, 0, raster.cells.Length);
            for (var i = 0; i < raster.heights.Length; i++)
            {
                raster.heights[i] = reader.ReadInt16();
            }

            return raster;
        }
        catch (EndOfStreamException)
        {
            return null;
        }
    }

    private int Index(int row, int column) => row * Size + column;

    private int Clamp(int index) => index < 0 ? 0 : index >= Size ? Size - 1 : index;

    private static short EncodeHeight(float height)
    {
        if (float.IsNaN(height))
        {
            return 0;
        }

        var decimetres = Math.Round(height * 10f);
        return (short)Math.Max(short.MinValue, Math.Min(short.MaxValue, decimetres));
    }
}

/// <summary>
/// Fills a <see cref="WorldMapRaster"/> a slice at a time so the caller can spread the work
/// over many Unity frames. The sampler is only ever called from <see cref="Step"/>.
/// </summary>
public sealed class WorldMapRasterBuilder
{
    private readonly Func<float, float, WorldMapSample> sampler;
    private int nextCell;

    public WorldMapRasterBuilder(int size, long worldKey, Func<float, float, WorldMapSample> sampler)
    {
        this.sampler = sampler ?? throw new ArgumentNullException(nameof(sampler));
        Raster = new WorldMapRaster(size, worldKey);
    }

    public WorldMapRaster Raster { get; }

    public bool IsComplete => nextCell >= Raster.Size * Raster.Size;

    public double Progress => (double)nextCell / (Raster.Size * Raster.Size);

    /// <summary>
    /// Samples cells until <paramref name="keepGoing"/> says stop (checked every 64 cells) or
    /// the raster is complete. Returns true when complete.
    /// </summary>
    public bool Step(Func<bool> keepGoing)
    {
        var total = Raster.Size * Raster.Size;
        while (nextCell < total)
        {
            var stop = Math.Min(total, nextCell + 64);
            for (; nextCell < stop; nextCell++)
            {
                var row = nextCell / Raster.Size;
                var column = nextCell % Raster.Size;
                var sample = sampler((float)Raster.CellCenter(column), (float)Raster.CellCenter(row));
                Raster.Set(row, column, sample);
            }

            if (!keepGoing())
            {
                break;
            }
        }

        return IsComplete;
    }
}

/// <summary>Turns raster samples into tile pixels, close to the in-game map's look.</summary>
public static class WorldMapTileRenderer
{
    private static readonly (byte R, byte G, byte B) OutsideWorld = (24, 32, 48);

    /// <summary>Renders one tile as packed RGB (row-major, row 0 = north).</summary>
    public static byte[] RenderRgb(WorldMapRaster raster, int zoom, int x, int y) =>
        Render(
            raster,
            WorldMapGeometry.TilePixels,
            WorldMapGeometry.TileWorldSize(zoom) / WorldMapGeometry.TilePixels,
            (column, row) => WorldMapGeometry.PixelCenter(zoom, x, y, column, row));

    /// <summary>Renders one protocol version 1 tile of <see cref="WorldMapFrame"/> as packed RGB.</summary>
    public static byte[] RenderFrameRgb(WorldMapRaster raster, int zoom, int x, int y) =>
        Render(
            raster,
            WorldMapFrame.TileSize,
            WorldMapFrame.TileWorldSize(zoom) / WorldMapFrame.TileSize,
            (column, row) => WorldMapFrame.PixelCenter(zoom, x, y, column, row));

    private static byte[] Render(
        WorldMapRaster raster,
        int n,
        double metresPerPixel,
        Func<int, int, (double WorldX, double WorldZ)> pixelCenter)
    {
        var rgb = new byte[n * n * 3];
        // Hill shading reads the slope over at least one raster cell.
        var slopeStep = Math.Max(metresPerPixel, raster.CellSize);

        for (var row = 0; row < n; row++)
        {
            for (var column = 0; column < n; column++)
            {
                var (wx, wz) = pixelCenter(column, row);
                var color = ColorAt(raster, wx, wz, slopeStep);
                var offset = (row * n + column) * 3;
                rgb[offset] = color.R;
                rgb[offset + 1] = color.G;
                rgb[offset + 2] = color.B;
            }
        }

        return rgb;
    }

    public static (byte R, byte G, byte B) ColorAt(WorldMapRaster raster, double wx, double wz, double slopeStep)
    {
        if (Math.Sqrt(wx * wx + wz * wz) > WorldMapGeometry.WorldEdgeRadius)
        {
            return OutsideWorld;
        }

        var height = raster.SampleHeight(wx, wz);
        if (height < WorldMapGeometry.WaterLevel)
        {
            // Shallow water is light, deep water dark, like the in-game map.
            var depth = Clamp01((WorldMapGeometry.WaterLevel - height) / 60f);
            return Mix((86, 150, 196), (20, 52, 102), depth);
        }

        var (row, column) = raster.CellOf(wx, wz);
        var biome = raster.BiomeAt(row, column);
        var baseColor = BiomeColor(biome);
        if (raster.ForestAt(row, column))
        {
            baseColor = Scale(baseColor, 0.78f);
        }

        // Light from the north-west, as on most maps.
        var east = raster.SampleHeight(wx + slopeStep, wz);
        var north = raster.SampleHeight(wx, wz + slopeStep);
        var dx = (east - height) / slopeStep;
        var dz = (north - height) / slopeStep;
        var shade = 1f + (float)(-dx * 0.6 + dz * 0.6);
        shade = Math.Max(0.55f, Math.Min(1.3f, shade));
        return Scale(baseColor, shade);
    }

    /// <summary>Biome colours from the game's Minimap defaults, softened for a map view.</summary>
    public static (byte R, byte G, byte B) BiomeColor(WorldMapBiome biome) => biome switch
    {
        WorldMapBiome.Meadows => (112, 178, 82),
        WorldMapBiome.BlackForest => (46, 104, 52),
        WorldMapBiome.Swamp => (122, 102, 86),
        WorldMapBiome.Mountain => (222, 226, 232),
        WorldMapBiome.Plains => (214, 196, 92),
        WorldMapBiome.Mistlands => (78, 74, 88),
        WorldMapBiome.AshLands => (156, 58, 44),
        WorldMapBiome.DeepNorth => (236, 242, 250),
        WorldMapBiome.Ocean => (86, 150, 196),
        _ => (128, 128, 128)
    };

    private static (byte R, byte G, byte B) Mix((byte R, byte G, byte B) a, (byte R, byte G, byte B) b, float t) =>
        ((byte)(a.R + (b.R - a.R) * t), (byte)(a.G + (b.G - a.G) * t), (byte)(a.B + (b.B - a.B) * t));

    private static (byte R, byte G, byte B) Scale((byte R, byte G, byte B) c, float factor) =>
        (ClampByte(c.R * factor), ClampByte(c.G * factor), ClampByte(c.B * factor));

    private static byte ClampByte(float value) => (byte)Math.Max(0, Math.Min(255, Math.Round(value)));

    private static float Clamp01(float value) => value < 0 ? 0 : value > 1 ? 1 : value;
}

/// <summary>Minimal PNG writer for 8-bit RGB images (no Unity dependency).</summary>
public static class PngEncoder
{
    private static readonly byte[] Signature = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];
    private static readonly uint[] CrcTable = BuildCrcTable();

    public static byte[] EncodeRgb(int width, int height, byte[] rgb, bool compress = true)
    {
        if (width <= 0 || height <= 0 || rgb.Length != width * height * 3)
        {
            throw new ArgumentException("RGB buffer does not match the image size.", nameof(rgb));
        }

        // Each scanline starts with filter type 0 (none).
        var stride = width * 3;
        var raw = new byte[(stride + 1) * height];
        for (var row = 0; row < height; row++)
        {
            Buffer.BlockCopy(rgb, row * stride, raw, row * (stride + 1) + 1, stride);
        }

        var ihdr = new byte[13];
        WriteBigEndian(ihdr, 0, (uint)width);
        WriteBigEndian(ihdr, 4, (uint)height);
        ihdr[8] = 8; // bit depth
        ihdr[9] = 2; // colour type: truecolour

        using var png = new MemoryStream();
        png.Write(Signature, 0, Signature.Length);
        WriteChunk(png, "IHDR", ihdr);
        WriteChunk(png, "IDAT", Zlib(raw, compress));
        WriteChunk(png, "IEND", []);
        return png.ToArray();
    }

    /// <summary>zlib stream: header, deflate body (stored blocks when compression is unavailable), Adler-32.</summary>
    public static byte[] Zlib(byte[] data, bool compress)
    {
        byte[]? body = null;
        if (compress)
        {
            try
            {
                using var deflated = new MemoryStream();
                using (var deflate = new DeflateStream(deflated, CompressionLevel.Optimal, leaveOpen: true))
                {
                    deflate.Write(data, 0, data.Length);
                }

                body = deflated.ToArray();
            }
            catch (Exception exception) when (exception is DllNotFoundException or EntryPointNotFoundException or TypeInitializationException)
            {
                body = null;
            }
        }

        body ??= StoredDeflate(data);

        using var zlib = new MemoryStream();
        zlib.WriteByte(0x78);
        zlib.WriteByte(0x01);
        zlib.Write(body, 0, body.Length);
        var adler = Adler32(data);
        var trailer = new byte[4];
        WriteBigEndian(trailer, 0, adler);
        zlib.Write(trailer, 0, 4);
        return zlib.ToArray();
    }

    public static uint Crc32(byte[] data, int offset, int count)
    {
        var crc = 0xFFFFFFFFu;
        for (var i = offset; i < offset + count; i++)
        {
            crc = CrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        }

        return crc ^ 0xFFFFFFFFu;
    }

    public static uint Adler32(byte[] data)
    {
        const uint mod = 65521;
        uint a = 1, b = 0;
        foreach (var value in data)
        {
            a = (a + value) % mod;
            b = (b + a) % mod;
        }

        return (b << 16) | a;
    }

    private static byte[] StoredDeflate(byte[] data)
    {
        using var stream = new MemoryStream();
        var offset = 0;
        do
        {
            var length = Math.Min(65535, data.Length - offset);
            var final = offset + length >= data.Length;
            stream.WriteByte((byte)(final ? 1 : 0));
            stream.WriteByte((byte)(length & 0xFF));
            stream.WriteByte((byte)(length >> 8));
            stream.WriteByte((byte)(~length & 0xFF));
            stream.WriteByte((byte)((~length >> 8) & 0xFF));
            stream.Write(data, offset, length);
            offset += length;
        }
        while (offset < data.Length);

        return stream.ToArray();
    }

    private static void WriteChunk(Stream stream, string type, byte[] data)
    {
        var header = new byte[8];
        WriteBigEndian(header, 0, (uint)data.Length);
        for (var i = 0; i < 4; i++)
        {
            header[4 + i] = (byte)type[i];
        }

        var crcInput = new byte[4 + data.Length];
        Buffer.BlockCopy(header, 4, crcInput, 0, 4);
        Buffer.BlockCopy(data, 0, crcInput, 4, data.Length);
        var crc = new byte[4];
        WriteBigEndian(crc, 0, Crc32(crcInput, 0, crcInput.Length));

        stream.Write(header, 0, header.Length);
        stream.Write(data, 0, data.Length);
        stream.Write(crc, 0, crc.Length);
    }

    private static void WriteBigEndian(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)(value >> 24);
        buffer[offset + 1] = (byte)(value >> 16);
        buffer[offset + 2] = (byte)(value >> 8);
        buffer[offset + 3] = (byte)value;
    }

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            var c = n;
            for (var k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }

            table[n] = c;
        }

        return table;
    }
}

/// <summary>
/// Answers <c>getMapInfo</c>/<c>getMapTile</c> from a published raster. Safe to call from any
/// thread: the raster is replaced atomically and never mutated after publication.
/// </summary>
public sealed class WorldMapService
{
    public const int DefaultTileCacheCapacity = 512;

    private readonly object cacheGate = new();
    // Frame marks a protocol version 1 tile; the two layouts share one cache.
    private readonly Dictionary<(bool Frame, int Z, int X, int Y), LinkedListNode<((bool Frame, int Z, int X, int Y) Key, string Png)>> cache = new();
    private readonly LinkedList<((bool Frame, int Z, int X, int Y) Key, string Png)> lru = new();
    private readonly int cacheCapacity;
    private WorldMapRaster? raster;
    private volatile string status = "the world has not loaded yet";

    public WorldMapService(int cacheCapacity = DefaultTileCacheCapacity)
    {
        this.cacheCapacity = Math.Max(1, cacheCapacity);
    }

    public bool IsReady => Volatile.Read(ref raster) is not null;

    public WorldMapRaster? Raster => Volatile.Read(ref raster);

    /// <summary>Why tiles are not available yet; shown in getMapTile errors.</summary>
    public void SetStatus(string value) => status = value;

    public void Publish(WorldMapRaster? next)
    {
        lock (cacheGate)
        {
            cache.Clear();
            lru.Clear();
        }

        Volatile.Write(ref raster, next);
    }

    public TakaroActionResult GetMapInfo() => TakaroActionResult.Ok(WorldMapGeometry.CreateMapInfo(IsReady));

    public TakaroActionResult GetMapTile(int x, int y, int zoom)
    {
        var current = Volatile.Read(ref raster);
        if (current is null)
        {
            return TakaroActionResult.Error("map_not_ready", $"Valheim world map is not ready: {status}.");
        }

        if (!WorldMapGeometry.IsValidZoom(zoom))
        {
            return TakaroActionResult.Error(
                "invalid_args",
                $"Map tile zoom {zoom} is outside 0..{WorldMapGeometry.MaxZoom}.");
        }

        if (!WorldMapGeometry.IntersectsWorld(zoom, x, y))
        {
            return TakaroActionResult.Error(
                "tile_out_of_range",
                $"Map tile {zoom}/{x}/{y} lies outside the Valheim world.");
        }

        return TakaroActionResult.Ok(CachedPng(
            current,
            (false, zoom, x, y),
            () => PngEncoder.EncodeRgb(WorldMapGeometry.TilePixels, WorldMapGeometry.TilePixels, WorldMapTileRenderer.RenderRgb(current, zoom, x, y))));
    }

    /// <summary>A protocol version 1 <c>getMapTile { dimension, zoom, x, y }</c> answer.</summary>
    public TakaroActionResult GetFrameTile(string dimension, int zoom, int x, int y)
    {
        if (dimension != WorldMapFrame.DimensionId)
        {
            return TakaroActionResult.Error("not_found", $"Valheim has no map dimension '{dimension}'.");
        }

        if (!WorldMapFrame.IsValidTile(zoom, x, y))
        {
            return TakaroActionResult.Error("invalid_args", $"Map tile {zoom}/{x}/{y} is outside the declared pyramid.");
        }

        var current = Volatile.Read(ref raster);
        if (current is null)
        {
            // not_found makes Takaro show a transparent tile and ask again later.
            return TakaroActionResult.Error("map_not_ready", $"Valheim world map is not ready: {status}.");
        }

        var png = CachedPng(
            current,
            (true, zoom, x, y),
            () => PngEncoder.EncodeRgb(WorldMapFrame.TileSize, WorldMapFrame.TileSize, WorldMapTileRenderer.RenderFrameRgb(current, zoom, x, y)));
        return TakaroActionResult.Ok(new { mimeType = "image/png", data = png });
    }

    private string CachedPng(WorldMapRaster current, (bool Frame, int Z, int X, int Y) key, Func<byte[]> render)
    {
        lock (cacheGate)
        {
            if (cache.TryGetValue(key, out var hit))
            {
                lru.Remove(hit);
                lru.AddFirst(hit);
                return hit.Value.Png;
            }
        }

        var png = Convert.ToBase64String(render());

        lock (cacheGate)
        {
            if (ReferenceEquals(Volatile.Read(ref raster), current) && !cache.ContainsKey(key))
            {
                cache[key] = lru.AddFirst((key, png));
                while (cache.Count > cacheCapacity)
                {
                    var last = lru.Last!;
                    lru.RemoveLast();
                    cache.Remove(last.Value.Key);
                }
            }
        }

        return png;
    }
}
