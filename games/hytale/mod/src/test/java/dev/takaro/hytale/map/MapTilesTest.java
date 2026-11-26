package dev.takaro.hytale.map;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Base64;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import static org.junit.jupiter.api.Assertions.*;

class MapTilesTest {

    private static final int GREEN = 0x22AA22FF;
    private static final int MARK = 0xFF00FFFF;

    private static int[] solid(int rgba) {
        int[] px = new int[ChunkRaster.CELLS];
        Arrays.fill(px, rgba);
        return px;
    }

    private static BufferedImage decode(String base64) throws Exception {
        return ImageIO.read(new ByteArrayInputStream(Base64.getDecoder().decode(base64)));
    }

    private static int argbOf(int rgba) {
        return (rgba & 0xFF) << 24 | (rgba >>> 8);
    }

    @Test
    void pngRoundTripsThroughImageIo() throws Exception {
        int[] px = new int[4 * 3];
        for (int i = 0; i < px.length; i++) {
            px[i] = (i * 20) << 24 | (255 - i * 20) << 16 | 7 << 8 | 0xFF;
        }
        BufferedImage img = ImageIO.read(new ByteArrayInputStream(PngEncoder.encodeRgba(4, 3, px)));
        assertEquals(4, img.getWidth());
        assertEquals(3, img.getHeight());
        assertEquals(argbOf(px[2 * 4 + 1]), img.getRGB(1, 2));
    }

    @Test
    void aBlockLandsOnThePixelThePlayerMapExpects() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        // Chunk (-2, 3) covers blocks x -64..-33, z 96..127; mark block (-50, 100).
        int[] px = solid(GREEN);
        px[(100 - 96) * 32 + (-50 + 64)] = MARK;
        raster.put(-2, 3, px, 1);
        MapTiles tiles = new MapTiles(raster, 16);

        int[] at = MapGeometry.locate(4, -50 + 0.5, 100 + 0.5);
        BufferedImage img = decode(tiles.tileBase64(4, at[0], at[1]));
        assertEquals(argbOf(MARK), img.getRGB(at[2], at[3]));
        assertEquals(argbOf(GREEN), img.getRGB(at[2] + 1, at[3]));
        // Unexplored blocks in the same tile stay transparent.
        int[] outside = MapGeometry.locate(4, 10.5, 100.5);
        assertEquals(0, img.getRGB(outside[2], outside[3]) >>> 24);
    }

    @Test
    void tilesWithoutExploredChunksAreTransparentPngs() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, solid(GREEN), 1);
        MapTiles tiles = new MapTiles(raster, 16);
        assertNotEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(4, 0, 0));
        assertNotEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(0, 0, 0));
        assertEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(4, 5, 5));
        assertEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(0, -1, -1));
        BufferedImage empty = decode(MapTiles.EMPTY_TILE_BASE64);
        assertEquals(128, empty.getWidth());
        assertEquals(128, empty.getHeight());
        for (int y = 0; y < 128; y += 7) {
            for (int x = 0; x < 128; x += 7) {
                assertEquals(0, empty.getRGB(x, y) >>> 24);
            }
        }
    }

    @Test
    void redrawnChunksInvalidateCachedTiles() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, solid(GREEN), 1);
        MapTiles tiles = new MapTiles(raster, 16);
        String before = tiles.tileBase64(4, 0, 0);
        raster.put(0, 0, solid(MARK), 2);
        String after = tiles.tileBase64(4, 0, 0);
        assertNotEquals(before, after);
        int[] at = MapGeometry.locate(4, 1.5, 1.5);
        assertEquals(argbOf(MARK), decode(after).getRGB(at[2], at[3]));
    }

    @Test
    void rasterSurvivesSaveAndLoadForTheSameWorldOnly(@TempDir Path dir) throws Exception {
        ChunkRaster raster = new ChunkRaster();
        raster.put(-3, 7, solid(MARK), 42);
        Path file = dir.resolve("worldmap-default.bin");
        raster.save(file, "1495...-a:99");
        assertFalse(raster.isDirty());

        ChunkRaster copy = new ChunkRaster();
        assertEquals(1, copy.load(file, "1495...-a:99"));
        assertEquals(MARK, copy.pixelAt(-3 * 32 + 4, 7 * 32 + 9));
        // Drawings from an earlier run count as stale, so loaded chunks are redrawn at once.
        assertEquals(0, copy.get(-3, 7).renderedAt());

        // A world re-created with the same name and seed has a new UUID: nothing is reused.
        ChunkRaster recreated = new ChunkRaster();
        assertEquals(-1, recreated.load(file, "77aa...-b:99"));
        assertEquals(0, recreated.size());
        assertEquals(-1, recreated.load(dir.resolve("missing.bin"), "1495...-a:99"));
    }

    @Test
    void cacheFromTheOldSeedKeyedFormatIsDiscarded(@TempDir Path dir) throws Exception {
        // Format 1 (an earlier build) keyed the file by seed*31 + name hash only.
        Path file = dir.resolve("worldmap-default.bin");
        try (java.io.DataOutputStream out = new java.io.DataOutputStream(
            new java.util.zip.GZIPOutputStream(java.nio.file.Files.newOutputStream(file)))) {
            out.writeInt(0x544B4D50);
            out.writeInt(1);
            out.writeLong(99L * 31 + "default".hashCode());
            out.writeInt(1);
            out.writeLong(ChunkRaster.key(0, 0));
            out.writeLong(5);
            for (int i = 0; i < ChunkRaster.CELLS; i++) {
                out.writeInt(GREEN);
            }
        }
        ChunkRaster raster = new ChunkRaster();
        assertEquals(-1, raster.load(file, "1495...-a:99"));
        assertEquals(0, raster.size());
    }

    @Test
    void chunksWipedFromTheSaveLoseTheirDrawing() throws Exception {
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, solid(GREEN), 1);
        raster.put(40, 40, solid(MARK), 1);
        MapTiles tiles = new MapTiles(raster, 16);
        int[] far = MapGeometry.locate(4, 40 * 32 + 0.5, 40 * 32 + 0.5);
        assertNotEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(4, far[0], far[1]));

        // After a chunk wipe only the spawn area is in the save again.
        assertEquals(1, raster.retainOnly(Set.of(ChunkRaster.key(0, 0), ChunkRaster.key(1, 0))));
        assertTrue(raster.isDirty());
        assertNull(raster.get(40, 40));
        assertNotNull(raster.get(0, 0));
        assertEquals(MapTiles.EMPTY_TILE_BASE64, tiles.tileBase64(4, far[0], far[1]), "cached tile dropped too");
        assertEquals(0, raster.retainOnly(Set.of(ChunkRaster.key(0, 0))));
    }

    @Test
    void reloadedChunksNearPlayersAreRedrawnImmediately(@TempDir Path dir) throws Exception {
        Set<Long> block = new HashSet<>();
        for (int x = -1; x <= 1; x++) {
            for (int z = -1; z <= 1; z++) {
                block.add(ChunkRaster.key(x, z));
            }
        }
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, solid(GREEN), 1_760_000_000_000L);
        Path file = dir.resolve("worldmap-default.bin");
        raster.save(file, "w:1");
        ChunkRaster reloaded = new ChunkRaster();
        reloaded.load(file, "w:1");
        // Five minutes have not passed since the drawing, but it comes from the earlier run.
        assertEquals(List.of(ChunkRaster.key(0, 0)),
            RenderPlanner.plan(block, block, reloaded, 1_760_000_000_500L, 300_000, 10, false));
    }

    @Test
    void maxAbsBlockCoversNegativeChunks() {
        ChunkRaster raster = new ChunkRaster();
        assertEquals(0, raster.maxAbsBlock());
        raster.put(-5, 2, solid(GREEN), 1);
        assertEquals(160, raster.maxAbsBlock());
    }

    @Test
    void plannerDrawsOnlyChunksWhoseNeighboursExist() {
        Set<Long> existing = new HashSet<>();
        for (int x = -2; x <= 2; x++) {
            for (int z = -2; z <= 2; z++) {
                existing.add(ChunkRaster.key(x, z));
            }
        }
        ChunkRaster raster = new ChunkRaster();
        List<Long> plan = RenderPlanner.plan(existing, Set.of(), raster, 0, 1000, 100, true);
        // Only the inner 3x3 has all eight neighbours; the border ring would make the
        // generator create new chunks.
        assertEquals(9, plan.size());
        for (Long key : plan) {
            assertTrue(Math.abs(ChunkRaster.keyX(key)) <= 1 && Math.abs(ChunkRaster.keyZ(key)) <= 1);
        }
        assertEquals(ChunkRaster.key(0, 0), plan.get(0), "nearest the origin first");
    }

    @Test
    void plannerRefreshesOnlyStaleActiveChunks() {
        Set<Long> existing = new HashSet<>();
        for (int x = -1; x <= 1; x++) {
            for (int z = -1; z <= 1; z++) {
                existing.add(ChunkRaster.key(x, z));
            }
        }
        ChunkRaster raster = new ChunkRaster();
        raster.put(0, 0, solid(GREEN), 1_000);
        long centre = ChunkRaster.key(0, 0);
        // All nine chunks are loaded (a player stands here).
        assertTrue(RenderPlanner.plan(existing, existing, raster, 1_500, 1_000, 10, false).isEmpty());
        assertEquals(List.of(centre), RenderPlanner.plan(existing, existing, raster, 2_000, 1_000, 10, false));
        assertTrue(RenderPlanner.plan(existing, Set.of(), raster, 2_000, 1_000, 10, false).isEmpty());
        // A loaded chunk whose neighbour is unloaded would load it: not refreshed.
        Set<Long> partly = new HashSet<>(existing);
        partly.remove(ChunkRaster.key(1, 1));
        assertTrue(RenderPlanner.plan(existing, partly, raster, 2_000, 1_000, 10, false).isEmpty());
    }

    @Test
    void plannerLoadsUnloadedChunksOnlyWhileTheWorldIsEmpty() {
        Set<Long> existing = new HashSet<>();
        for (int x = -3; x <= 3; x++) {
            for (int z = -3; z <= 3; z++) {
                existing.add(ChunkRaster.key(x, z));
            }
        }
        // A player near (2, 2) keeps chunks 1..3 loaded; only (2, 2) has its whole 3x3 loaded.
        Set<Long> loaded = new HashSet<>();
        for (int x = 1; x <= 3; x++) {
            for (int z = 1; z <= 3; z++) {
                loaded.add(ChunkRaster.key(x, z));
            }
        }
        ChunkRaster raster = new ChunkRaster();
        assertEquals(List.of(ChunkRaster.key(2, 2)), RenderPlanner.plan(existing, loaded, raster, 0, 1000, 100, false));
        assertEquals(25, RenderPlanner.plan(existing, loaded, raster, 0, 1000, 100, true).size());
    }
}
