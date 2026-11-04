using System;
using System.IO;
using System.IO.Compression;

namespace TakaroEmpyrion.Map
{
    /// <summary>
    /// Minimal PNG writer (8-bit RGBA, Sub filter, zlib via DeflateStream). Runs on worker threads only; Unity's
    /// own encoder needs the game thread. If the runtime has no native zlib, it falls back to stored deflate blocks,
    /// which are larger but still valid PNG.
    /// </summary>
    public static class Png
    {
        static readonly uint[] CrcTable = BuildCrcTable();
        static volatile bool _noDeflate;

        public static byte[] EncodeRgba(byte[] rgba, int width, int height)
        {
            if (rgba == null || rgba.Length != width * height * 4) throw new ArgumentException("rgba must hold width*height*4 bytes");
            int stride = width * 4;
            var raw = new byte[(stride + 1) * height];
            for (int y = 0; y < height; y++)
            {
                int src = y * stride, dst = y * (stride + 1);
                raw[dst] = 1; // Sub: each byte minus the byte one pixel to the left
                for (int i = 0; i < stride; i++)
                    raw[dst + 1 + i] = (byte)(rgba[src + i] - (i >= 4 ? rgba[src + i - 4] : 0));
            }
            byte[] z = Zlib(raw);

            using (var ms = new MemoryStream(z.Length + 128))
            {
                ms.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, 0, 8);
                var ihdr = new byte[13];
                WriteBE(ihdr, 0, (uint)width);
                WriteBE(ihdr, 4, (uint)height);
                ihdr[8] = 8;  // bit depth
                ihdr[9] = 6;  // RGBA
                Chunk(ms, "IHDR", ihdr);
                Chunk(ms, "IDAT", z);
                Chunk(ms, "IEND", new byte[0]);
                return ms.ToArray();
            }
        }

        static byte[] Zlib(byte[] data)
        {
            using (var ms = new MemoryStream(data.Length / 2 + 64))
            {
                ms.WriteByte(0x78);
                ms.WriteByte(0x9C);
                bool done = false;
                if (!_noDeflate)
                {
                    try
                    {
                        using (var d = new DeflateStream(ms, CompressionMode.Compress, true)) d.Write(data, 0, data.Length);
                        done = true;
                    }
                    catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is TypeInitializationException || e is NotSupportedException)
                    {
                        _noDeflate = true;
                        ms.SetLength(2);
                        ms.Position = 2;
                    }
                }
                if (!done) WriteStored(ms, data);
                uint adler = Adler32(data);
                var t = new byte[4];
                WriteBE(t, 0, adler);
                ms.Write(t, 0, 4);
                return ms.ToArray();
            }
        }

        static void WriteStored(Stream s, byte[] data)
        {
            int off = 0;
            do
            {
                int n = Math.Min(65535, data.Length - off);
                bool last = off + n >= data.Length;
                s.WriteByte((byte)(last ? 1 : 0));
                s.WriteByte((byte)(n & 0xFF)); s.WriteByte((byte)(n >> 8));
                s.WriteByte((byte)(~n & 0xFF)); s.WriteByte((byte)((~n >> 8) & 0xFF));
                s.Write(data, off, n);
                off += n;
            } while (off < data.Length);
        }

        static void Chunk(Stream s, string type, byte[] data)
        {
            var len = new byte[4];
            WriteBE(len, 0, (uint)data.Length);
            s.Write(len, 0, 4);
            var head = new byte[] { (byte)type[0], (byte)type[1], (byte)type[2], (byte)type[3] };
            s.Write(head, 0, 4);
            s.Write(data, 0, data.Length);
            uint crc = 0xFFFFFFFF;
            crc = Crc(crc, head, head.Length);
            crc = Crc(crc, data, data.Length);
            var c = new byte[4];
            WriteBE(c, 0, crc ^ 0xFFFFFFFF);
            s.Write(c, 0, 4);
        }

        public static uint Crc32(byte[] data, int offset, int count)
        {
            uint crc = 0xFFFFFFFF;
            for (int i = offset; i < offset + count; i++) crc = CrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFF;
        }

        static uint Crc(uint crc, byte[] data, int count)
        {
            for (int i = 0; i < count; i++) crc = CrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc;
        }

        static uint Adler32(byte[] data)
        {
            uint a = 1, b = 0;
            int i = 0;
            while (i < data.Length)
            {
                int n = Math.Min(5552, data.Length - i);
                for (int k = 0; k < n; k++) { a += data[i++]; b += a; }
                a %= 65521; b %= 65521;
            }
            return (b << 16) | a;
        }

        static uint[] BuildCrcTable()
        {
            var t = new uint[256];
            for (uint n = 0; n < 256; n++)
            {
                uint c = n;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
                t[n] = c;
            }
            return t;
        }

        static void WriteBE(byte[] b, int o, uint v)
        {
            b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v;
        }
    }
}
