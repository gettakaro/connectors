package dev.takaro.hytale.map;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

/** Minimal RGBA PNG writer (8-bit, no interlace, filter 0), so tiles need no java.desktop. */
public final class PngEncoder {
    private static final byte[] SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    private PngEncoder() {
    }

    /** Encode packed RGBA pixels ({@code r<<24|g<<16|b<<8|a}), row-major, top row first. */
    public static byte[] encodeRgba(int width, int height, int[] rgba) {
        if (rgba.length != width * height) {
            throw new IllegalArgumentException("pixel count does not match " + width + "x" + height);
        }
        byte[] raw = new byte[height * (1 + width * 4)];
        int p = 0;
        for (int y = 0; y < height; y++) {
            raw[p++] = 0;
            for (int x = 0; x < width; x++) {
                int c = rgba[y * width + x];
                raw[p++] = (byte) (c >>> 24);
                raw[p++] = (byte) (c >>> 16);
                raw[p++] = (byte) (c >>> 8);
                raw[p++] = (byte) c;
            }
        }

        Deflater deflater = new Deflater(6);
        deflater.setInput(raw);
        deflater.finish();
        ByteArrayOutputStream idat = new ByteArrayOutputStream(raw.length / 4 + 64);
        byte[] buf = new byte[16384];
        while (!deflater.finished()) {
            idat.write(buf, 0, deflater.deflate(buf));
        }
        deflater.end();

        ByteArrayOutputStream out = new ByteArrayOutputStream(idat.size() + 64);
        out.writeBytes(SIGNATURE);
        byte[] ihdr = new byte[13];
        putInt(ihdr, 0, width);
        putInt(ihdr, 4, height);
        ihdr[8] = 8;  // bit depth
        ihdr[9] = 6;  // colour type RGBA
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", idat.toByteArray());
        chunk(out, "IEND", new byte[0]);
        return out.toByteArray();
    }

    private static void chunk(ByteArrayOutputStream out, String type, byte[] data) {
        byte[] len = new byte[4];
        putInt(len, 0, data.length);
        out.writeBytes(len);
        byte[] typeBytes = type.getBytes(StandardCharsets.US_ASCII);
        out.writeBytes(typeBytes);
        out.writeBytes(data);
        CRC32 crc = new CRC32();
        crc.update(typeBytes);
        crc.update(data);
        byte[] c = new byte[4];
        putInt(c, 0, (int) crc.getValue());
        out.writeBytes(c);
    }

    private static void putInt(byte[] b, int off, int v) {
        b[off] = (byte) (v >>> 24);
        b[off + 1] = (byte) (v >>> 16);
        b[off + 2] = (byte) (v >>> 8);
        b[off + 3] = (byte) v;
    }
}
