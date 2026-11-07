using System.IO.Compression;

namespace TakaroUeExport;

/// <summary>A fixed-settings PNG writer: the same pixels give the same bytes, and no metadata chunk is written.</summary>
internal static class Png
{
    private static readonly uint[] CrcTable = BuildCrcTable();

    public static byte[] Encode(byte[] rgba, int width, int height, bool grayscale)
    {
        var bytesPerPixel = grayscale ? 1 : 4;
        var stride = width * bytesPerPixel;
        var raw = new byte[(stride + 1) * height];
        for (var y = 0; y < height; y++)
        {
            var row = y * (stride + 1);
            raw[row] = 0;
            for (var x = 0; x < width; x++)
            {
                var source = (y * width + x) * 4;
                if (grayscale)
                {
                    raw[row + 1 + x] = rgba[source];
                }
                else
                {
                    Array.Copy(rgba, source, raw, row + 1 + x * 4, 4);
                }
            }
        }

        using var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.SmallestSize, leaveOpen: true))
        {
            zlib.Write(raw, 0, raw.Length);
        }

        using var png = new MemoryStream();
        png.Write([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
        var header = new byte[13];
        WriteBigEndian(header, 0, (uint)width);
        WriteBigEndian(header, 4, (uint)height);
        header[8] = 8;
        header[9] = grayscale ? (byte)0 : (byte)6;
        WriteChunk(png, "IHDR", header);
        WriteChunk(png, "IDAT", compressed.ToArray());
        WriteChunk(png, "IEND", []);
        return png.ToArray();
    }

    private static void WriteChunk(Stream stream, string type, byte[] data)
    {
        var length = new byte[4];
        WriteBigEndian(length, 0, (uint)data.Length);
        stream.Write(length);
        var body = new byte[4 + data.Length];
        for (var i = 0; i < 4; i++)
        {
            body[i] = (byte)type[i];
        }

        Array.Copy(data, 0, body, 4, data.Length);
        stream.Write(body);
        var crc = new byte[4];
        WriteBigEndian(crc, 0, Crc32(body));
        stream.Write(crc);
    }

    private static void WriteBigEndian(byte[] target, int offset, uint value)
    {
        target[offset] = (byte)(value >> 24);
        target[offset + 1] = (byte)(value >> 16);
        target[offset + 2] = (byte)(value >> 8);
        target[offset + 3] = (byte)value;
    }

    private static uint Crc32(byte[] data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var value in data)
        {
            crc = CrcTable[(crc ^ value) & 0xFF] ^ (crc >> 8);
        }

        return crc ^ 0xFFFFFFFFu;
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
