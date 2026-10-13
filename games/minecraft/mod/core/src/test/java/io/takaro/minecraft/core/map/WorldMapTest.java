package io.takaro.minecraft.core.map;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class WorldMapTest {

    @TempDir
    Path dir;

    private static int rgb(int argb) {
        return argb & 0xffffff;
    }

    private static int shade(int color, int brightness) {
        return (((color >> 16) & 0xff) * brightness / 255 << 16)
                | (((color >> 8) & 0xff) * brightness / 255 << 8)
                | ((color & 0xff) * brightness / 255);
    }

    @Test
    void rendersTheTopBlockWithVanillaShading() throws Exception {
        // Flat grass at y=64 in chunk (0,0) of region (0,0); glass on top of block (0,0).
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:grass_block", (x, z) -> 64, true));
        Path file = dir.resolve("r.0.0.mca");
        TestRegions.writeRegion(file, chunks);

        int[] px = new RegionRenderer(TestRegions.colors()).render(RegionFile.read(file), false);

        // Flat ground: the checkerboard term alone stays inside NORMAL (220).
        assertEquals(shade(TestRegions.GRASS, 220), rgb(px[5 * 512 + 5]));
        // The glass at (0,0) is see-through: the grass below it is drawn.
        assertEquals(shade(TestRegions.GRASS, 220), rgb(px[0]));
        assertEquals(0xff, px[0] >>> 24);
        // Chunk (1,0) was never generated: transparent.
        assertEquals(0, px[20]);
    }

    @Test
    void shadesSlopesAgainstTheNorthernNeighbour() throws Exception {
        // Ground rises one block per row southwards: every row is higher than its northern one.
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:stone", (x, z) -> 60 + z, false));
        Path file = dir.resolve("r.0.0.mca");
        TestRegions.writeRegion(file, chunks);

        int[] px = new RegionRenderer(TestRegions.colors()).render(RegionFile.read(file), false);

        // d = 1 * 4/5 + 0.2 on odd squares = 1.0 > 0.6: HIGH.
        assertEquals(shade(TestRegions.STONE, 255), rgb(px[4 * 512 + 3]), "a rise is HIGH");
    }

    @Test
    void shadesWaterByDepth() throws Exception {
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        // Water surface at y=62 over stone: the column below the surface is stone, depth 0.
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:water", (x, z) -> 62, false));
        Path file = dir.resolve("r.0.0.mca");
        TestRegions.writeRegion(file, chunks);

        int[] px = new RegionRenderer(TestRegions.colors()).render(RegionFile.read(file), false);

        // depth 0: d = 0 (+0.2 on odd squares), both below 0.5, so HIGH.
        assertEquals(shade(TestRegions.WATER, 255), rgb(px[0]));
        assertEquals(shade(TestRegions.WATER, 255), rgb(px[1]));
    }

    @Test
    void heightmapDecodingMatchesPacking() {
        long[] packed = new long[37];
        for (int i = 0; i < 256; i++) packed[i / 7] |= (long) (i + 100) << ((i % 7) * 9);
        for (int i = 0; i < 256; i++) assertEquals(i + 100, RegionRenderer.heightmapValue(packed, i));
    }

    @Test
    void frameCoversExploredRegionsAsAPowerOfTwoSquare() throws Exception {
        Path regions = dir.resolve("region");
        Files.createDirectories(regions);
        for (String name : List.of("r.-1.-1.mca", "r.0.0.mca", "r.4.0.mca")) {
            Files.write(regions.resolve(name), new byte[9000]);
        }
        Files.write(regions.resolve("r.9.9.mca"), new byte[8192]); // header only: ignored

        WorldMap.Frame frame = WorldMap.computeFrame(regions);

        // x spans regions -1..4 (6), z spans -1..0 (2): an 8-region square, 4096 blocks.
        assertEquals(4096, frame.size());
        assertEquals(-2 * 512, frame.minX()); // -1 - (8 - 6) / 2
        assertEquals(-4 * 512, frame.minZ()); // -1 - (8 - 2) / 2
        // 4096 / 256 = 16 = 2^4 blocks per pixel at zoom 0; zoom 4 is 1:1, zoom 5 is 2 px per block.
        assertEquals(5, frame.maxZoom());
    }

    @Test
    void frameStaysPutWhileTheWorldStillFitsAndGrowsWhenItDoesNot() throws Exception {
        Path regions = dir.resolve("region");
        Files.createDirectories(regions);
        for (String name : List.of("r.-1.-1.mca", "r.0.0.mca")) Files.write(regions.resolve(name), new byte[9000]);
        WorldMap.Frame first = WorldMap.computeFrame(regions, null);
        assertEquals(new WorldMap.Frame(-1024, -1024, 2048, 4), first);

        // A new region inside the frame: same frame, no re-centring.
        Files.write(regions.resolve("r.1.0.mca"), new byte[9000]);
        assertSame(first, WorldMap.computeFrame(regions, first));
        // Re-computed from scratch it would have moved.
        assertNotEquals(first, WorldMap.computeFrame(regions, null));

        // A region outside it: the frame grows to cover everything.
        Files.write(regions.resolve("r.5.0.mca"), new byte[9000]);
        WorldMap.Frame grown = WorldMap.computeFrame(regions, first);
        assertEquals(4096, grown.size());
        assertTrue(grown.minX() <= -512 && grown.maxX() >= 6 * 512);
    }

    @Test
    void emptyWorldGetsADefaultFrameAroundSpawn() {
        WorldMap.Frame frame = WorldMap.computeFrame(dir.resolve("missing"));
        assertEquals(new WorldMap.Frame(-1024, -1024, 2048, 4), frame);
    }

    @Test
    void servesPngTilesInTheTopLeftAnchoredPyramid() throws Exception {
        Path regions = dir.resolve("region");
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:grass_block", (x, z) -> 64, false));
        TestRegions.writeRegion(regions.resolve("r.0.0.mca"), chunks);
        List<String> log = new ArrayList<>();
        WorldMap map = new WorldMap(List.of(new WorldMap.Dimension("overworld", "Overworld", regions, false)),
                TestRegions.colors(), log::add);
        try {
            WorldMap.Frame frame = map.frame("overworld");
            // One region, centred in the minimum 4-region square.
            assertEquals(new WorldMap.Frame(-512, -512, 2048, 4), frame);

            // Zoom 4: 16x16 tiles of 128 blocks. Block (0,0) is tile (4,4), pixel (0,0).
            byte[] png = map.tile("overworld", 4, 4, 4, 5000);
            assertArrayEquals(new byte[] {(byte) 0x89, 'P', 'N', 'G'}, java.util.Arrays.copyOf(png, 4));
            int[] pixels = decode(png);
            assertEquals(shade(TestRegions.GRASS, 220), rgb(pixels[0]));
            assertEquals(0xff, pixels[0] >>> 24);
            // 2 px per block: the chunk's 16 blocks are 32 px; pixel 40 is unexplored.
            assertEquals(0, pixels[40] >>> 24);

            // A tile with nothing in it is a valid, fully transparent PNG.
            int[] empty = decode(map.tile("overworld", 4, 0, 0, 5000));
            for (int p : empty) assertEquals(0, p >>> 24);

            assertThrows(IllegalArgumentException.class, () -> map.tile("overworld", 5, 0, 0, 5000));
            assertThrows(IllegalArgumentException.class, () -> map.tile("overworld", 1, 2, 0, 5000));
            assertThrows(IllegalArgumentException.class, () -> map.tile("nether", 0, 0, 0, 5000));

            List<Map<String, Object>> declaration = map.declaration();
            assertEquals(1, declaration.size());
            assertEquals("overworld", declaration.get(0).get("id"));
            assertEquals(Map.of("field", "z", "direction", "down"),
                    ((Map<?, ?>) declaration.get(0).get("axes")).get("vertical"));
            assertEquals(Map.of("type", "tiled", "tileSize", 256, "minZoom", 0, "maxZoom", 4),
                    declaration.get(0).get("source"));
        } finally {
            map.close();
        }
    }

    @Test
    void rereadsARegionAfterTheServerSavesIt() throws Exception {
        Path regions = dir.resolve("region");
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:grass_block", (x, z) -> 64, false));
        Path file = regions.resolve("r.0.0.mca");
        TestRegions.writeRegion(file, chunks);
        WorldMap map = new WorldMap(List.of(new WorldMap.Dimension("overworld", "Overworld", regions, false)),
                TestRegions.colors(), s -> {});
        try {
            assertEquals(shade(TestRegions.GRASS, 220), rgb(decode(map.tile("overworld", 4, 4, 4, 5000))[0]));
            chunks.put(0, TestRegions.chunk(0, 0, "minecraft:stone", (x, z) -> 64, false));
            TestRegions.writeRegion(file, chunks);
            Files.setLastModifiedTime(file, java.nio.file.attribute.FileTime.fromMillis(System.currentTimeMillis() + 5000));
            // Served at once from the old drawing while the region is redrawn behind it...
            assertEquals(shade(TestRegions.GRASS, 220), rgb(decode(map.tile("overworld", 4, 4, 4, 5000))[0]));
            // ...and once the redraw lands, requests get the saved change.
            int pixel = 0;
            for (int i = 0; i < 100 && pixel != shade(TestRegions.STONE, 220); i++) {
                Thread.sleep(50);
                pixel = rgb(decode(map.tile("overworld", 4, 4, 4, 5000))[0]);
            }
            assertEquals(shade(TestRegions.STONE, 220), pixel);
        } finally {
            map.close();
        }
    }

    @Test
    void forgetsARegionTheServerDeleted() throws Exception {
        Path regions = dir.resolve("region");
        Map<Integer, Map<String, Object>> chunks = new HashMap<>();
        chunks.put(0, TestRegions.chunk(0, 0, "minecraft:grass_block", (x, z) -> 64, false));
        Path file = regions.resolve("r.0.0.mca");
        TestRegions.writeRegion(file, chunks);
        // A second region keeps the frame where it is once r.0.0 is gone.
        TestRegions.writeRegion(regions.resolve("r.-1.-1.mca"), chunks);
        WorldMap map = new WorldMap(List.of(new WorldMap.Dimension("overworld", "Overworld", regions, false)),
                TestRegions.colors(), s -> {});
        try {
            // Frame -1024..1024: block (0,0) is tile (8,8) at zoom 4 and pixel (128,128) at zoom 0.
            assertEquals(shade(TestRegions.GRASS, 220), rgb(decode(map.tile("overworld", 4, 8, 8, 5000))[0]));
            assertEquals(shade(TestRegions.GRASS, 220), rgb(decode(map.tile("overworld", 0, 0, 0, 5000))[128 * 256 + 128]));

            // Trimmed or reset area: the region file is removed while the server runs.
            Files.delete(file);

            // Every zoom stops showing it at once, with no redraw to wait for; r.-1.-1 stays.
            for (int p : decode(map.tile("overworld", 4, 8, 8, 5000))) assertEquals(0, p >>> 24);
            assertEquals(0, decode(map.tile("overworld", 0, 0, 0, 5000))[128 * 256 + 128] >>> 24);
            assertEquals(shade(TestRegions.GRASS, 220), rgb(decode(map.tile("overworld", 4, 4, 4, 5000))[0]));

            // Regenerated later: it is drawn again from the new file.
            chunks.put(0, TestRegions.chunk(0, 0, "minecraft:stone", (x, z) -> 64, false));
            TestRegions.writeRegion(file, chunks);
            assertEquals(shade(TestRegions.STONE, 220), rgb(decode(map.tile("overworld", 4, 8, 8, 5000))[0]));
        } finally {
            map.close();
        }
    }

    /** Decodes the PNGs PngWriter writes (RGBA, filter 0) with the JDK's own reader. */
    static int[] decode(byte[] png) throws Exception {
        java.awt.image.BufferedImage img = javax.imageio.ImageIO.read(new java.io.ByteArrayInputStream(png));
        assertEquals(256, img.getWidth());
        assertEquals(256, img.getHeight());
        return img.getRGB(0, 0, 256, 256, null, 0, 256);
    }
}
