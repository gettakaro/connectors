package dev.takaro.hytale.map;

import com.hypixel.hytale.server.core.universe.world.chunk.palette.BitFieldArr;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class MapImageDecoderTest {

    private static final int RED = 0xFF0000FF;
    private static final int BLUE = 0x0000FFFF;

    /** Pack indices exactly as the server's ImageBuilder does. */
    private static byte[] pack(int bits, int[] indices) {
        BitFieldArr arr = new BitFieldArr(bits, indices.length);
        for (int i = 0; i < indices.length; i++) {
            arr.set(i, indices[i]);
        }
        return arr.get();
    }

    @Test
    void readIndexMatchesTheServerBitField() {
        for (int bits : new int[]{4, 8, 12, 16}) {
            int n = 300;
            int[] values = new int[n];
            for (int i = 0; i < n; i++) {
                values[i] = (i * 2654435761L % (1L << bits)) > 0 ? (int) (i * 2654435761L % (1L << bits)) : i;
            }
            byte[] packed = pack(bits, values);
            BitFieldArr reference = new BitFieldArr(bits, n);
            reference.set(packed);
            for (int i = 0; i < n; i++) {
                assertEquals(reference.get(i), MapImageDecoder.readIndex(packed, bits, i), "bits " + bits + " index " + i);
            }
        }
    }

    @Test
    void ninetySixPixelChunkBecomesOnePixelPerBlock() {
        // Left half red, right half blue, at the 3 px per block the world generator uses.
        int w = 96;
        int[] idx = new int[w * w];
        for (int y = 0; y < w; y++) {
            for (int x = 0; x < w; x++) {
                idx[y * w + x] = x < 48 ? 0 : 1;
            }
        }
        int[] px = MapImageDecoder.toChunkPixels(w, w, new int[]{RED, BLUE}, 4, pack(4, idx));
        assertNotNull(px);
        assertEquals(32 * 32, px.length);
        assertEquals(RED, px[0]);
        assertEquals(RED, px[15]);
        assertEquals(BLUE, px[16]);
        assertEquals(BLUE, px[31 * 32 + 31]);
    }

    @Test
    void rowsAreZAndColumnsAreX() {
        // Only image row 0 (z = 0) is blue.
        int w = 32;
        int[] idx = new int[w * w];
        for (int x = 0; x < w; x++) {
            idx[x] = 1;
        }
        int[] px = MapImageDecoder.toChunkPixels(w, w, new int[]{RED, BLUE}, 4, pack(4, idx));
        assertEquals(BLUE, px[5]);        // z 0, x 5
        assertEquals(RED, px[32 + 5]);    // z 1, x 5
    }

    @Test
    void ditheredCellsAreAveraged() {
        int w = 64; // 2x2 pixels per block
        int[] idx = new int[w * w];
        for (int i = 0; i < idx.length; i++) {
            idx[i] = ((i % w) + (i / w)) % 2;
        }
        int[] px = MapImageDecoder.toChunkPixels(w, w, new int[]{0x000000FF, 0xC8C8C8FF}, 4, pack(4, idx));
        assertEquals(0x646464FF, px[0]);
    }

    @Test
    void malformedImagesAreRejected() {
        assertNull(MapImageDecoder.toChunkPixels(32, 32, null, 4, new byte[512]));
        assertNull(MapImageDecoder.toChunkPixels(32, 32, new int[]{RED}, 4, new byte[10]));
        int[] idx = new int[32 * 32];
        idx[3] = 5; // palette has 2 entries
        assertNull(MapImageDecoder.toChunkPixels(32, 32, new int[]{RED, BLUE}, 4, pack(4, idx)));
    }
}
