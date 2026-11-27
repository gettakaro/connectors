package dev.takaro.hytale.map;

/**
 * Turns one Hytale {@code MapImage} (the chunk picture the server sends to clients for the
 * in-game map) into the raster's 32x32, one-pixel-per-block form.
 *
 * <p>A {@code MapImage} is a palette of packed RGBA colours plus palette indices packed
 * little-end-first at {@code bitsPerIndex} bits each (Hytale's {@code BitFieldArr}), pixel
 * {@code z * width + x}. The world map draws a 32-block chunk at {@code 32 * imageScale}
 * pixels (96 for the default world generator), so each block averages its pixel cell, which
 * also smooths the generator's ordered dithering.
 */
public final class MapImageDecoder {
    private MapImageDecoder() {
    }

    /** Index {@code i} from a packed index array, the way {@code BitFieldArr.get} reads it. */
    static int readIndex(byte[] packed, int bits, int i) {
        long bitIndex = (long) i * bits;
        int byteIndex = (int) (bitIndex >>> 3);
        int bitOffset = (int) (bitIndex & 7);
        int value = 0;
        int shift = 0;
        int remaining = bits;
        while (remaining > 0) {
            int inThisByte = Math.min(8 - bitOffset, remaining);
            int mask = (1 << inThisByte) - 1;
            value |= (((packed[byteIndex] & 0xFF) >>> bitOffset) & mask) << shift;
            shift += inThisByte;
            remaining -= inThisByte;
            byteIndex++;
            bitOffset = 0;
        }
        return value;
    }

    /**
     * Decode to 32x32 packed RGBA. Returns null when the image is malformed (missing palette or
     * indices, too few index bytes, or an index outside the palette).
     */
    public static int[] toChunkPixels(int width, int height, int[] palette, int bitsPerIndex, byte[] packed) {
        int n = MapGeometry.CHUNK_SIZE;
        if (palette == null || packed == null || palette.length == 0 || width < 1 || height < 1
            || bitsPerIndex < 1 || bitsPerIndex > 16) {
            return null;
        }
        long neededBytes = ((long) width * height * bitsPerIndex + 7) / 8;
        if (packed.length < neededBytes) {
            return null;
        }
        int[] out = new int[n * n];
        for (int bz = 0; bz < n; bz++) {
            int y0 = bz * height / n;
            int y1 = Math.max(y0 + 1, (bz + 1) * height / n);
            for (int bx = 0; bx < n; bx++) {
                int x0 = bx * width / n;
                int x1 = Math.max(x0 + 1, (bx + 1) * width / n);
                long r = 0, g = 0, b = 0, a = 0;
                int count = 0;
                for (int y = y0; y < y1; y++) {
                    for (int x = x0; x < x1; x++) {
                        int idx = readIndex(packed, bitsPerIndex, y * width + x);
                        if (idx >= palette.length) {
                            return null;
                        }
                        int c = palette[idx];
                        r += (c >>> 24) & 0xFF;
                        g += (c >>> 16) & 0xFF;
                        b += (c >>> 8) & 0xFF;
                        a += c & 0xFF;
                        count++;
                    }
                }
                out[bz * n + bx] = (int) ((r / count) << 24 | (g / count) << 16 | (b / count) << 8 | (a / count));
            }
        }
        return out;
    }
}
