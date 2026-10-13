using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using Terraria;
using Terraria.Map;
using TShockAPI;

namespace TakaroTerrariaEvents;

// Renders the loaded world into one PNG with the game's own map colours (MapHelper), the
// same palette the in-game full-screen map uses, and writes it with a small JSON sidecar
// into <TShock save path>/takaro/. The bridge serves that file to Takaro as the map image.
//
// The render reads Main.tile from a background thread. It never writes game state, so a
// tile that changes mid-render only shows up one revision later; the game thread is never
// held up for it.
internal static class MapRenderer
{
    // Takaro accepts map images up to 8192 px a side; a large world (8400 tiles wide) is
    // sampled every second tile.
    internal const int MaxImageSide = 8192;

    private static readonly object Gate = new();
    private static Thread? _running;
    private static string? _lastRevision;

    internal static string OutputDirectory => Path.Combine(TShock.SavePath, "takaro");

    // Starts a render unless one is already running. Returns false when one is.
    internal static bool Start(string reason)
    {
        lock (Gate)
        {
            if (_running is { IsAlive: true }) return false;
            _running = new Thread(() => Run(reason))
            {
                IsBackground = true,
                Priority = ThreadPriority.BelowNormal,
                Name = "takaro-map-render",
            };
            _running.Start();
            return true;
        }
    }

    private static void Run(string reason)
    {
        try
        {
            var watch = System.Diagnostics.Stopwatch.StartNew();
            var result = RenderToDisk();
            TShock.Log.ConsoleInfo(
                $"TAKARO_MAP {(result.Changed ? "rendered" : "unchanged")} revision={result.Revision} " +
                $"{result.Width}x{result.Height} tiles={Main.maxTilesX}x{Main.maxTilesY} bytes={result.Size} " +
                $"ms={watch.ElapsedMilliseconds} reason={reason}");
        }
        catch (Exception ex)
        {
            TShock.Log.ConsoleError($"TAKARO_MAP failed: {ex.GetType().Name}: {ex.Message}");
        }
    }

    internal readonly record struct RenderResult(string Revision, int Width, int Height, long Size, bool Changed);

    private static RenderResult RenderToDisk()
    {
        EnsureColors();
        int tilesX = Main.maxTilesX, tilesY = Main.maxTilesY;
        int step = SampleStep(tilesX, tilesY);
        int width = (tilesX + step - 1) / step, height = (tilesY + step - 1) / step;

        var rgb = new byte[width * height * 3];
        for (int py = 0; py < height; py++)
        {
            int j = py * step;
            int row = py * width * 3;
            for (int px = 0; px < width; px++)
            {
                int i = px * step;
                var color = TileColor(i, j);
                int o = row + px * 3;
                rgb[o] = color.R;
                rgb[o + 1] = color.G;
                rgb[o + 2] = color.B;
            }
        }

        var png = PngWriter.EncodeRgb(width, height, rgb);
        var revision = Convert.ToHexString(SHA256.HashData(png)).Substring(0, 16).ToLowerInvariant();
        if (revision == _lastRevision && File.Exists(Path.Combine(OutputDirectory, "map.json")))
        {
            return new RenderResult(revision, width, height, png.LongLength, false);
        }

        Directory.CreateDirectory(OutputDirectory);
        // The image first, then the metadata naming it: a reader that sees map.json always
        // finds the bytes it describes. Each file is replaced by a rename, never rewritten.
        var imageName = $"map-{revision}.png";
        WriteAtomically(Path.Combine(OutputDirectory, imageName), png);
        var meta = new
        {
            revision,
            file = imageName,
            mimeType = "image/png",
            size = png.LongLength,
            width,
            height,
            // Player positions are Terraria world pixels (16 per tile), x right and y down.
            bounds = new
            {
                x = new { min = 0, max = tilesX * 16 },
                y = new { min = 0, max = tilesY * 16 },
            },
            tilesX,
            tilesY,
            worldName = Main.worldName,
            worldId = Main.worldID,
            renderedAt = DateTime.UtcNow.ToString("o"),
        };
        WriteAtomically(Path.Combine(OutputDirectory, "map.json"), JsonSerializer.SerializeToUtf8Bytes(meta));
        foreach (var old in Directory.GetFiles(OutputDirectory, "map-*.png"))
        {
            if (Path.GetFileName(old) != imageName) File.Delete(old);
        }
        _lastRevision = revision;
        return new RenderResult(revision, width, height, png.LongLength, true);
    }

    internal static int SampleStep(int tilesX, int tilesY)
    {
        int longest = Math.Max(tilesX, tilesY);
        return Math.Max(1, (longest + MaxImageSide - 1) / MaxImageSide);
    }

    // A dedicated server never opens the map, so the lookup tables may not exist yet.
    // Initialize fills colorLookup before it builds the map atlas, so a failure in that
    // last client-only step still leaves the colours usable.
    private static void EnsureColors()
    {
        if (MapHelper.colorLookup != null) return;
        try
        {
            MapHelper.Initialize();
        }
        catch (Exception ex) when (MapHelper.colorLookup != null)
        {
            TShock.Log.ConsoleInfo($"TAKARO_MAP colour tables ready (map atlas skipped: {ex.GetType().Name})");
        }
    }

    // The full-light colour of one tile, as the in-game map draws a revealed tile.
    private static Microsoft.Xna.Framework.Color TileColor(int i, int j)
    {
        // CreateMapTile falls back to GetBackgroundType for empty tiles, which reads the
        // client-only Main.Map for snow; the background is chosen here instead.
        var tile = MapHelper.CreateMapTile(i, j, byte.MaxValue, BackgroundType(j));
        return MapHelper.GetMapTileXnaColor(tile, i, j);
    }

    private static int BackgroundType(int j)
    {
        if (j < Main.worldSurface)
        {
            return MapHelper.skyPosition + (byte)(255 * (j / Main.worldSurface));
        }
        if (j >= Main.UnderworldLayer) return MapHelper.hellPosition;
        return j < Main.rockLayer ? MapHelper.dirtPosition : MapHelper.rockPosition;
    }

    private static void WriteAtomically(string path, byte[] bytes)
    {
        var temp = path + ".tmp";
        File.WriteAllBytes(temp, bytes);
        File.Move(temp, path, overwrite: true);
    }
}

// Minimal PNG encoder: 8-bit RGB, one zlib stream, filter type 0 (None) on every row.
internal static class PngWriter
{
    private static readonly uint[] CrcTable = BuildCrcTable();

    internal static byte[] EncodeRgb(int width, int height, byte[] rgb)
    {
        if (rgb.Length != width * height * 3) throw new ArgumentException("pixel buffer does not match the size");
        using var output = new MemoryStream();
        output.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });

        var header = new byte[13];
        WriteBigEndian(header, 0, (uint)width);
        WriteBigEndian(header, 4, (uint)height);
        header[8] = 8; // bit depth
        header[9] = 2; // colour type RGB
        WriteChunk(output, "IHDR", header);

        using (var compressed = new MemoryStream())
        {
            using (var zlib = new ZLibStream(compressed, CompressionLevel.Optimal, leaveOpen: true))
            {
                int stride = width * 3;
                for (int y = 0; y < height; y++)
                {
                    zlib.WriteByte(0);
                    zlib.Write(rgb, y * stride, stride);
                }
            }
            WriteChunk(output, "IDAT", compressed.ToArray());
        }
        WriteChunk(output, "IEND", Array.Empty<byte>());
        return output.ToArray();
    }

    private static void WriteChunk(Stream output, string type, byte[] data)
    {
        var length = new byte[4];
        WriteBigEndian(length, 0, (uint)data.Length);
        output.Write(length);
        var typeBytes = System.Text.Encoding.ASCII.GetBytes(type);
        output.Write(typeBytes);
        output.Write(data);
        uint crc = Crc(Crc(0xFFFFFFFFu, typeBytes), data) ^ 0xFFFFFFFFu;
        var crcBytes = new byte[4];
        WriteBigEndian(crcBytes, 0, crc);
        output.Write(crcBytes);
    }

    private static uint Crc(uint crc, byte[] data)
    {
        foreach (var b in data) crc = CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }

    private static void WriteBigEndian(byte[] buffer, int offset, uint value)
    {
        buffer[offset] = (byte)(value >> 24);
        buffer[offset + 1] = (byte)(value >> 16);
        buffer[offset + 2] = (byte)(value >> 8);
        buffer[offset + 3] = (byte)value;
    }
}
