package dev.takaro.hytale.map;

import org.junit.jupiter.api.Test;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.util.Arrays;
import java.util.Map;
import java.util.regex.Pattern;

import static org.junit.jupiter.api.Assertions.*;

class MapPyramidTest {

    private static final int GREEN = 0x22AA22FF;
    private static final int MARK = 0xFF00FFFF;
    private static final Pattern DIMENSION_ID = Pattern.compile("^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$");

    private static int argbOf(int rgba) {
        return (rgba & 0xFF) << 24 | (rgba >>> 8);
    }

    private static BufferedImage decode(byte[] png) throws Exception {
        return ImageIO.read(new ByteArrayInputStream(png));
    }

    @Test
    void anEmptyMapIsCentredOnTheOrigin() {
        MapPyramid.Frame f = MapPyramid.frameFor(null, null);
        assertEquals(new MapPyramid.Frame(-1024, -1024, 2048, 4), f);
        // 2048 blocks over 8 tiles of 256 px at zoom 3 is one block per pixel; zoom 4 is two.
        assertEquals(256, f.span(3));
        assertEquals(128, f.span(4));
    }

    @Test
    void theFrameIsASquareHoldingEveryDrawnChunk() {
        // Chunks x -10..40, z -5..5: 1632 blocks wide.
        MapPyramid.Frame f = MapPyramid.frameFor(new int[]{-10, 40, -5, 5}, null);
        assertEquals(2048, f.size());
        assertTrue(f.contains(-10 * 32, -5 * 32));
        assertTrue(f.contains(41 * 32 - 1, 6 * 32 - 1));
        assertEquals(0, Math.floorMod(f.minX(), 32), "on whole chunks");
        assertEquals(0, Math.floorMod(f.minZ(), 32));
    }

    @Test
    void theFrameGrowsInPowersOfTwo() {
        MapPyramid.Frame f = MapPyramid.frameFor(new int[]{0, 99, 0, 0}, null); // 3200 blocks
        assertEquals(4096, f.size());
        assertEquals(5, f.maxZoom());
        assertTrue(f.contains(0, 0));
        assertTrue(f.contains(100 * 32 - 1, 31));
    }

    @Test
    void aFrameThatStillFitsIsKeptSoTileCoordinatesStayStable() {
        MapPyramid.Frame first = MapPyramid.frameFor(new int[]{0, 1, 0, 1}, null);
        assertSame(first, MapPyramid.frameFor(new int[]{-3, 4, -2, 5}, first));
        assertSame(first, MapPyramid.frameFor(null, first));
        MapPyramid.Frame grown = MapPyramid.frameFor(new int[]{-200, 4, -2, 5}, first);
        assertNotEquals(first, grown);
        assertTrue(grown.contains(-200 * 32, 0));
    }

    @Test
    void tileCoordinatesFollowTakarosPyramid() {
        MapPyramid.Frame f = new MapPyramid.Frame(-1024, -1024, 2048, 4);
        assertTrue(f.isValidTile(0, 0, 0));
        assertFalse(f.isValidTile(0, 1, 0));
        assertTrue(f.isValidTile(4, 15, 15));
        assertFalse(f.isValidTile(4, 16, 0));
        assertFalse(f.isValidTile(5, 0, 0));
        assertFalse(f.isValidTile(-1, 0, 0));
        assertFalse(f.isValidTile(2, -1, 0));
        // Zoom 0: one tile shows the whole frame, top-left is the north-west corner.
        assertEquals(-1024, f.blockX(0, 0, 0));
        assertEquals(-1024, f.blockZ(0, 0, 0));
        assertEquals(-1024 + 255 * 8, f.blockX(0, 0, 255));
        // Row grows southwards (+Z).
        assertEquals(-1024 + 8, f.blockZ(0, 0, 1));
        // Deepest zoom: two pixels per block.
        assertEquals(f.blockX(4, 8, 0), f.blockX(4, 8, 1));
        assertEquals(0, f.blockX(4, 8, 0));
        assertEquals(1, f.blockX(4, 8, 2));
    }

    @Test
    void declarationIsATiledDimension() {
        MapPyramid.Frame f = new MapPyramid.Frame(-1024, -2048, 4096, 5);
        Map<String, Object> d = MapPyramid.declaration("default", "default", f);
        assertEquals("default", d.get("id"));
        assertEquals("default", d.get("name"));
        assertEquals(Map.of(
            "horizontal", Map.of("field", "x", "direction", "right"),
            "vertical", Map.of("field", "z", "direction", "down")), d.get("axes"));
        assertEquals(Map.of(
            "horizontal", Map.of("min", -1024, "max", 3072),
            "vertical", Map.of("min", -2048, "max", 2048)), d.get("bounds"));
        assertEquals(Map.of("type", "tiled", "tileSize", 256, "minZoom", 0, "maxZoom", 5), d.get("source"));
    }

    @Test
    void dimensionIdsAreValidProtocolIds() {
        assertEquals("default", MapPyramid.dimensionId("default"));
        assertEquals("My_World", MapPyramid.dimensionId("My World"));
        assertEquals("w_hidden", MapPyramid.dimensionId("_hidden"));
        assertEquals("default", MapPyramid.dimensionId(null));
        assertEquals("default", MapPyramid.dimensionId("  "));
        assertEquals(64, MapPyramid.dimensionId("x".repeat(100)).length());
        for (String name : new String[]{"default", "Orbis 2", "-x", "é", "a:b.c-d_e", "x".repeat(80)}) {
            assertTrue(DIMENSION_ID.matcher(MapPyramid.dimensionId(name)).matches(), name);
        }
    }

    @Test
    void aBlockLandsOnTheRightPixelOfAPyramidTile() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        // Chunk (1, 2) covers blocks x 32..63, z 64..95; mark block (40, 70).
        int[] px = new int[ChunkRaster.CELLS];
        Arrays.fill(px, GREEN);
        px[(70 - 64) * 32 + (40 - 32)] = MARK;
        raster.put(1, 2, px, 1);
        MapTiles tiles = new MapTiles(raster, 16);
        MapPyramid.Frame f = MapPyramid.frameFor(raster.bounds(), null);

        // Deepest zoom: 128 blocks per tile, two pixels per block.
        int zoom = f.maxZoom();
        int span = f.span(zoom);
        int tx = (40 - f.minX()) / span;
        int ty = (70 - f.minZ()) / span;
        int col = (40 - f.minX() - tx * span) * 2;
        int row = (70 - f.minZ() - ty * span) * 2;
        BufferedImage img = decode(tiles.pyramidTilePng(f, zoom, tx, ty));
        assertEquals(256, img.getWidth());
        assertEquals(argbOf(MARK), img.getRGB(col, row));
        assertEquals(argbOf(MARK), img.getRGB(col + 1, row + 1));
        assertEquals(argbOf(GREEN), img.getRGB(col + 2, row));
        // North of the marked block (smaller z) is one row up.
        assertEquals(argbOf(GREEN), img.getRGB(col, row - 2));

        // Zoom 0 shows the drawn chunk too.
        BufferedImage whole = decode(tiles.pyramidTilePng(f, 0, 0, 0));
        int wc = (int) ((40L - f.minX()) * 256 / f.size());
        int wr = (int) ((64L - f.minZ()) * 256 / f.size()) + 1;
        assertEquals(argbOf(GREEN), whole.getRGB(wc, wr));
    }

    @Test
    void aTileWithNothingDrawnIsTransparent() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, new int[ChunkRaster.CELLS], 1);
        MapTiles tiles = new MapTiles(raster, 16);
        MapPyramid.Frame f = new MapPyramid.Frame(-1024, -1024, 2048, 4);
        assertFalse(tiles.hasData(f, 4, 0, 0));
        BufferedImage img = decode(tiles.pyramidTilePng(f, 4, 0, 0));
        assertEquals(256, img.getWidth());
        assertEquals(256, img.getHeight());
        for (int y = 0; y < 256; y += 9) {
            for (int x = 0; x < 256; x += 9) {
                assertEquals(0, img.getRGB(x, y) >>> 24);
            }
        }
    }

    @Test
    void aRedrawnChunkInvalidatesCachedPyramidTiles() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        int[] green = new int[ChunkRaster.CELLS];
        Arrays.fill(green, GREEN);
        raster.put(0, 0, green, 1);
        MapTiles tiles = new MapTiles(raster, 16);
        MapPyramid.Frame f = MapPyramid.frameFor(raster.bounds(), null);
        byte[] before = tiles.pyramidTilePng(f, 0, 0, 0);
        assertSame(before, tiles.pyramidTilePng(f, 0, 0, 0), "cached");
        int[] mark = new int[ChunkRaster.CELLS];
        Arrays.fill(mark, MARK);
        raster.put(0, 0, mark, 2);
        assertFalse(Arrays.equals(before, tiles.pyramidTilePng(f, 0, 0, 0)));
    }

    @Test
    void rasterBoundsCoverEveryDrawnChunk() {
        ChunkRaster raster = new ChunkRaster();
        assertNull(raster.bounds());
        raster.put(-3, 7, new int[ChunkRaster.CELLS], 1);
        raster.put(5, -2, new int[ChunkRaster.CELLS], 1);
        assertArrayEquals(new int[]{-3, 5, -2, 7}, raster.bounds());
    }
}
