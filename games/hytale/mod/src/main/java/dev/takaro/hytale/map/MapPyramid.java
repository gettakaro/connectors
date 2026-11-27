package dev.takaro.hytale.map;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Tile layout for Takaro protocol v1 ({@code map} capability, {@code getMapTile {dimension, zoom,
 * x, y}}). The v0 layout in {@link MapGeometry} is unrelated and stays as it is.
 *
 * <p>Takaro's pyramid: the declared bounds are a square frame, zoom 0 is one tile showing the
 * whole frame, and zoom {@code z} splits it into {@code 2^z x 2^z} tiles of {@link #TILE_SIZE}
 * pixels, tile (0, 0) at the top-left. Hytale's +X is east and +Z is south, and its in-game map
 * draws +Z downwards, so the frame's top-left is its north-west corner (min X, min Z) and the
 * axes are declared as x right, z down: the Takaro map then looks like the in-game one.
 *
 * <p>The frame covers whole chunks and grows in powers of two as the world is explored; it is
 * kept while it still holds everything drawn, so tile coordinates only change when the world
 * outgrows the map. At the deepest zoom one block is two pixels.
 *
 * <p>Free of Hytale imports so it can be unit-tested without the server jar.
 */
public final class MapPyramid {
    public static final int TILE_SIZE = 256;
    /** Smallest frame edge in blocks, so a fresh world still gets a usable map. */
    public static final int MIN_SIDE = 2048;
    /** Largest frame edge in blocks; anything drawn farther out is cut off. */
    public static final int MAX_SIDE = 1 << 22;

    private static final Pattern DIMENSION_ID = Pattern.compile("^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$");

    /** The square the pyramid covers, in blocks: x grows east (right), z grows south (down). */
    public record Frame(int minX, int minZ, int size, int maxZoom) {
        public int maxX() {
            return minX + size;
        }

        public int maxZ() {
            return minZ + size;
        }

        /** Blocks across one tile at {@code zoom}. */
        public int span(int zoom) {
            return size >> zoom;
        }

        public boolean isValidTile(int zoom, int x, int y) {
            if (zoom < 0 || zoom > maxZoom) {
                return false;
            }
            int n = 1 << zoom;
            return x >= 0 && y >= 0 && x < n && y < n;
        }

        /** Block X sampled for pixel column {@code px} of tile column {@code x}. */
        public long blockX(int zoom, int x, int px) {
            int span = span(zoom);
            return (long) minX + (long) x * span + (long) px * span / TILE_SIZE;
        }

        /** Block Z sampled for pixel row {@code py} of tile row {@code y}; row 0 is the north edge. */
        public long blockZ(int zoom, int y, int py) {
            int span = span(zoom);
            return (long) minZ + (long) y * span + (long) py * span / TILE_SIZE;
        }

        /** True when the block lies inside the frame. */
        public boolean contains(long blockX, long blockZ) {
            return blockX >= minX && blockX < maxX() && blockZ >= minZ && blockZ < maxZ();
        }
    }

    private MapPyramid() {
    }

    /** The frame for an empty map: {@link #MIN_SIDE} centred on the world origin. */
    public static Frame defaultFrame() {
        return frameOf(-MIN_SIDE / 2, -MIN_SIDE / 2, MIN_SIDE);
    }

    /**
     * The frame for the drawn chunk rectangle {@code bounds = {minCx, maxCx, minCz, maxCz}}
     * (inclusive; null when nothing is drawn). A {@code previous} frame that still holds every
     * drawn chunk is kept.
     */
    public static Frame frameFor(int[] bounds, Frame previous) {
        if (bounds == null) {
            return previous != null ? previous : defaultFrame();
        }
        long minBx = (long) bounds[0] * MapGeometry.CHUNK_SIZE;
        long maxBx = ((long) bounds[1] + 1) * MapGeometry.CHUNK_SIZE; // exclusive
        long minBz = (long) bounds[2] * MapGeometry.CHUNK_SIZE;
        long maxBz = ((long) bounds[3] + 1) * MapGeometry.CHUNK_SIZE;
        if (previous != null && minBx >= previous.minX() && minBz >= previous.minZ()
            && maxBx <= previous.maxX() && maxBz <= previous.maxZ()) {
            return previous;
        }
        long extent = Math.max(maxBx - minBx, maxBz - minBz);
        int side = MIN_SIDE;
        while (side < extent && side < MAX_SIDE) {
            side *= 2;
        }
        // Centre the drawn area in the square, on whole chunks.
        long padX = (side - (maxBx - minBx)) / 2;
        long padZ = (side - (maxBz - minBz)) / 2;
        long ox = minBx - Math.floorDiv(Math.max(0, padX), MapGeometry.CHUNK_SIZE) * MapGeometry.CHUNK_SIZE;
        long oz = minBz - Math.floorDiv(Math.max(0, padZ), MapGeometry.CHUNK_SIZE) * MapGeometry.CHUNK_SIZE;
        return frameOf(clampInt(ox), clampInt(oz), side);
    }

    private static Frame frameOf(int minX, int minZ, int side) {
        // One block per pixel where a tile spans TILE_SIZE blocks; one zoom deeper gives two.
        int maxZoom = Integer.numberOfTrailingZeros(side / TILE_SIZE) + 1;
        return new Frame(minX, minZ, side, maxZoom);
    }

    private static int clampInt(long v) {
        return (int) Math.max(Integer.MIN_VALUE / 2, Math.min(Integer.MAX_VALUE / 2, v));
    }

    /** A Takaro Dimension id from a world name: the protocol allows {@code [A-Za-z0-9_.:-]}, 64 long. */
    public static String dimensionId(String worldName) {
        if (worldName == null || worldName.isBlank()) {
            return "default";
        }
        String id = worldName.trim().replaceAll("[^A-Za-z0-9_.:-]", "_");
        if (!Character.isLetterOrDigit(id.charAt(0)) || id.charAt(0) > 0x7F) {
            id = "w" + id;
        }
        if (id.length() > 64) {
            id = id.substring(0, 64);
        }
        return DIMENSION_ID.matcher(id).matches() ? id : "default";
    }

    /** The {@code tiled} map Dimension declaration for one world. */
    public static Map<String, Object> declaration(String id, String name, Frame frame) {
        Map<String, Object> dim = new LinkedHashMap<>();
        dim.put("id", id);
        dim.put("name", name == null || name.isBlank() ? id : name);
        dim.put("axes", Map.of(
            "horizontal", Map.of("field", "x", "direction", "right"),
            "vertical", Map.of("field", "z", "direction", "down")));
        dim.put("bounds", Map.of(
            "horizontal", Map.of("min", frame.minX(), "max", frame.maxX()),
            "vertical", Map.of("min", frame.minZ(), "max", frame.maxZ())));
        Map<String, Object> source = new LinkedHashMap<>();
        source.put("type", "tiled");
        source.put("tileSize", TILE_SIZE);
        source.put("minZoom", 0);
        source.put("maxZoom", frame.maxZoom());
        dim.put("source", source);
        return dim;
    }
}
