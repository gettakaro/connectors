package io.takaro.minecraft.core.map;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.DeflaterOutputStream;

/** Writes ARGB pixels as an 8-bit RGBA PNG. Plain Java, so it needs no AWT on a headless server. */
public final class PngWriter {

    private static final byte[] SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    private PngWriter() {}

    public static byte[] write(int[] argb, int width, int height) {
        try {
            ByteArrayOutputStream raw = new ByteArrayOutputStream();
            Deflater deflater = new Deflater(6);
            try (DeflaterOutputStream z = new DeflaterOutputStream(raw, deflater)) {
                byte[] row = new byte[1 + width * 4];
                for (int y = 0; y < height; y++) {
                    row[0] = 0; // filter: none
                    for (int x = 0; x < width; x++) {
                        int p = argb[y * width + x];
                        int o = 1 + x * 4;
                        row[o] = (byte) (p >> 16);
                        row[o + 1] = (byte) (p >> 8);
                        row[o + 2] = (byte) p;
                        row[o + 3] = (byte) (p >>> 24);
                    }
                    z.write(row);
                }
            } finally {
                deflater.end();
            }

            ByteArrayOutputStream out = new ByteArrayOutputStream(raw.size() + 64);
            out.write(SIGNATURE);
            ByteArrayOutputStream ihdr = new ByteArrayOutputStream();
            DataOutputStream h = new DataOutputStream(ihdr);
            h.writeInt(width);
            h.writeInt(height);
            h.writeByte(8); // bit depth
            h.writeByte(6); // colour type RGBA
            h.writeByte(0);
            h.writeByte(0);
            h.writeByte(0);
            chunk(out, "IHDR", ihdr.toByteArray());
            chunk(out, "IDAT", raw.toByteArray());
            chunk(out, "IEND", new byte[0]);
            return out.toByteArray();
        } catch (IOException e) {
            throw new IllegalStateException("PNG encoding failed", e);
        }
    }

    private static void chunk(ByteArrayOutputStream out, String type, byte[] data) throws IOException {
        DataOutputStream d = new DataOutputStream(out);
        byte[] typeBytes = type.getBytes(StandardCharsets.US_ASCII);
        d.writeInt(data.length);
        d.write(typeBytes);
        d.write(data);
        CRC32 crc = new CRC32();
        crc.update(typeBytes);
        crc.update(data);
        d.writeInt((int) crc.getValue());
    }
}
