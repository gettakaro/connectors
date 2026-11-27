package dev.takaro.hytale.map;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Tile layout served through {@code getMapInfo}/{@code getMapTile} (Takaro protocol v0).
 *
 * <p>It follows the 7 Days to Die web-map convention that Takaro and the player map render:
 * signed tile indices centred on the world origin, 128 px tiles, tile y grows with +Z, and at
 * {@link #MAX_ZOOM} one pixel is one block. At zoom z one tile spans {@code 128 * 2^(4 - z)}
 * blocks. Row 0 of a tile image is its +Z edge.
 *
 * <p>Hytale itself draws +Z towards the bottom of its in-game map (south), so a viewer that
 * follows this convention shows the map mirrored north-south compared with the game.
 *
 * <p>Free of Hytale imports so it can be unit-tested without the server jar.
 */
public final class MapGeometry {
    public static final int TILE_PIXELS = 128;
    public static final int MAX_ZOOM = 4;
    /** Hytale chunk edge in blocks. */
    public static final int CHUNK_SIZE = 32;
    /** World height in blocks (10 sections of 32). */
    public static final int WORLD_HEIGHT = 320;
    /** Smallest world extent reported in map info, so a fresh world still gets a usable view. */
    public static final int MIN_EXTENT = 4096;

    private MapGeometry() {
    }

    public static boolean isValidZoom(int zoom) {
        return zoom >= 0 && zoom <= MAX_ZOOM;
    }

    /** Blocks covered by one tile edge at the given zoom. */
    public static int tileBlocks(int zoom) {
        return TILE_PIXELS << (MAX_ZOOM - zoom);
    }

    /** Blocks per pixel at the given zoom. */
    public static int blocksPerPixel(int zoom) {
        return 1 << (MAX_ZOOM - zoom);
    }

    /** Smallest block X covered by tile x. */
    public static long tileMinX(int zoom, int x) {
        return (long) x * tileBlocks(zoom);
    }

    /** Smallest block Z covered by tile y. */
    public static long tileMinZ(int zoom, int y) {
        return (long) y * tileBlocks(zoom);
    }

    /** Block X sampled for pixel column {@code col} of tile x (the pixel's first block). */
    public static long pixelBlockX(int zoom, int x, int col) {
        return tileMinX(zoom, x) + (long) col * blocksPerPixel(zoom);
    }

    /** Block Z sampled for pixel row {@code row} of tile y; row 0 is the +Z edge. */
    public static long pixelBlockZ(int zoom, int y, int row) {
        return tileMinZ(zoom, y) + tileBlocks(zoom) - (long) (row + 1) * blocksPerPixel(zoom);
    }

    /** The tile (x, y) and pixel (col, row) that contain block (bx, bz). */
    public static int[] locate(int zoom, double blockX, double blockZ) {
        int size = tileBlocks(zoom);
        int bpp = blocksPerPixel(zoom);
        int tileX = (int) Math.floor(blockX / size);
        int tileY = (int) Math.floor(blockZ / size);
        int col = (int) Math.floor((blockX - (double) tileX * size) / bpp);
        int rowFromBottom = (int) Math.floor((blockZ - (double) tileY * size) / bpp);
        int row = TILE_PIXELS - 1 - rowFromBottom;
        return new int[]{tileX, tileY, Math.min(col, TILE_PIXELS - 1), Math.max(0, Math.min(row, TILE_PIXELS - 1))};
    }

    /** Chunk coordinate that contains a block coordinate. */
    public static int chunkOf(long block) {
        return (int) Math.floorDiv(block, CHUNK_SIZE);
    }

    /** Chunk range [min, max] (inclusive) a tile covers on one axis, given the tile's first block. */
    public static int[] chunkRange(long minBlock, int zoom) {
        return new int[]{chunkOf(minBlock), chunkOf(minBlock + tileBlocks(zoom) - 1)};
    }

    /**
     * World extent reported in map info: twice the largest explored distance from the origin,
     * rounded up to a whole zoom-0 tile and never below {@link #MIN_EXTENT}.
     */
    public static int extentFor(int maxAbsBlock) {
        long needed = Math.max(MIN_EXTENT, 2L * Math.max(0, maxAbsBlock));
        long step = tileBlocks(0);
        long rounded = ((needed + step - 1) / step) * step;
        return (int) Math.min(rounded, 1 << 26);
    }

    /**
     * Payload for Takaro's MapInfoDTO. {@code mapBlockSize} is the world size of one tile at
     * {@code maxZoom}. {@code mapSizeY} is the build height, so readers that infer the up axis
     * from the smallest extent see a Y-up world. {@code axes} uses the protocol-v1 Dimension shape.
     */
    public static Map<String, Object> mapInfo(boolean enabled, int extent) {
        Map<String, Object> info = new LinkedHashMap<>();
        info.put("enabled", enabled);
        info.put("mapBlockSize", TILE_PIXELS);
        info.put("maxZoom", MAX_ZOOM);
        info.put("mapSizeX", extent);
        info.put("mapSizeY", WORLD_HEIGHT);
        info.put("mapSizeZ", extent);
        // Not part of Takaro's MapInfoDTO: the protocol-v1 Dimension axes, telling a viewer that
        // Hytale draws +Z downwards (south), so it can match the in-game map's orientation.
        info.put("axes", Map.of(
            "horizontal", Map.of("field", "x", "direction", "right"),
            "vertical", Map.of("field", "z", "direction", "down")));
        return info;
    }
}
