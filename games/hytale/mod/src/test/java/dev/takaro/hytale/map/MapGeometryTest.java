package dev.takaro.hytale.map;

import org.junit.jupiter.api.Test;

import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class MapGeometryTest {

    /**
     * The player map's Leaflet maths for a legacy (v0) map with mapBlockSize 128: lat = z/128,
     * lng = x/128, CRS.Simple scale 2^(zoom+3), pixel = (lng*scale, -lat*scale), and the tile it
     * requests is (floor(px/128), -floor(py/128) - 1). Returns {tileX, tileY, col, row}.
     */
    private static int[] trackerLocate(int zoom, double blockX, double blockZ) {
        double scale = Math.pow(2, zoom + 3);
        double px = blockX / 128.0 * scale;
        double py = -(blockZ / 128.0) * scale;
        int leafletX = (int) Math.floor(px / 128);
        int leafletY = (int) Math.floor(py / 128);
        int col = (int) Math.floor(px - leafletX * 128.0);
        int row = (int) Math.floor(py - leafletY * 128.0);
        return new int[]{leafletX, -leafletY - 1, col, row};
    }

    @Test
    void tileAndPixelMatchThePlayerMapAtEveryZoom() {
        double[][] points = {{0.5, 0.5}, {-0.5, -0.5}, {123.4, -456.7}, {-1000.2, 777.9}, {2047.5, -2048.5}, {5.0, 127.5}};
        for (int zoom = 0; zoom <= MapGeometry.MAX_ZOOM; zoom++) {
            for (double[] p : points) {
                int[] ours = MapGeometry.locate(zoom, p[0], p[1]);
                int[] tracker = trackerLocate(zoom, p[0], p[1]);
                assertArrayEquals(tracker, ours, "zoom " + zoom + " point " + p[0] + "," + p[1]);
            }
        }
    }

    @Test
    void pixelBlockRoundTripsThroughLocate() {
        for (int zoom = 0; zoom <= MapGeometry.MAX_ZOOM; zoom++) {
            for (int[] t : new int[][]{{0, 0}, {-1, -1}, {3, -2}}) {
                for (int[] px : new int[][]{{0, 0}, {127, 127}, {5, 90}}) {
                    long bx = MapGeometry.pixelBlockX(zoom, t[0], px[0]);
                    long bz = MapGeometry.pixelBlockZ(zoom, t[1], px[1]);
                    int[] back = MapGeometry.locate(zoom, bx + 0.5, bz + 0.5);
                    assertArrayEquals(new int[]{t[0], t[1], px[0], px[1]}, back);
                }
            }
        }
    }

    @Test
    void rowZeroIsThePositiveZEdge() {
        // Tile y=0 at max zoom covers z 0..127: its top row is z 127, its bottom row z 0.
        assertEquals(127, MapGeometry.pixelBlockZ(4, 0, 0));
        assertEquals(0, MapGeometry.pixelBlockZ(4, 0, 127));
        assertEquals(-128, MapGeometry.pixelBlockZ(4, -1, 127));
    }

    @Test
    void tileSizesFollowTheZoomPyramid() {
        assertEquals(2048, MapGeometry.tileBlocks(0));
        assertEquals(128, MapGeometry.tileBlocks(4));
        assertEquals(16, MapGeometry.blocksPerPixel(0));
        assertEquals(1, MapGeometry.blocksPerPixel(4));
        assertFalse(MapGeometry.isValidZoom(5));
        assertFalse(MapGeometry.isValidZoom(-1));
    }

    @Test
    void chunkOfFloorsNegativeBlocks() {
        assertEquals(0, MapGeometry.chunkOf(0));
        assertEquals(0, MapGeometry.chunkOf(31));
        assertEquals(-1, MapGeometry.chunkOf(-1));
        assertEquals(-1, MapGeometry.chunkOf(-32));
        assertEquals(-2, MapGeometry.chunkOf(-33));
    }

    @Test
    void mapInfoReportsAYUpWorldWithOneBlockPerPixel() {
        Map<String, Object> info = MapGeometry.mapInfo(true, MapGeometry.extentFor(1000));
        assertEquals(true, info.get("enabled"));
        assertEquals(128, info.get("mapBlockSize"));
        assertEquals(4, info.get("maxZoom"));
        assertEquals(4096, info.get("mapSizeX"));
        assertEquals(320, info.get("mapSizeY"));
        assertEquals(4096, info.get("mapSizeZ"));
        assertTrue((int) info.get("mapSizeY") < (int) info.get("mapSizeZ"), "smallest extent is the up axis (Y)");
        assertEquals(Map.of("horizontal", Map.of("field", "x", "direction", "right"),
            "vertical", Map.of("field", "z", "direction", "down")), info.get("axes"));
    }

    @Test
    void extentGrowsWithTheExploredArea() {
        assertEquals(4096, MapGeometry.extentFor(0));
        assertEquals(4096, MapGeometry.extentFor(2048));
        assertEquals(6144, MapGeometry.extentFor(2049));
    }
}
