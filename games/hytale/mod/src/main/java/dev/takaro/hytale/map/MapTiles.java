package dev.takaro.hytale.map;

import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Cuts {@link MapGeometry} tiles out of a {@link ChunkRaster}. Pure CPU work on the caller's
 * thread; it never touches the game. Unexplored blocks stay transparent.
 *
 * <p>Encoded tiles are kept in a small LRU keyed by the raster revision, so a re-rendered chunk
 * invalidates every cached tile at once.
 */
public final class MapTiles {
    /** Answer for tiles with no explored terrain: viewers cache it instead of retrying an error. */
    public static final String EMPTY_TILE_BASE64 = Base64.getEncoder().encodeToString(
        PngEncoder.encodeRgba(MapGeometry.TILE_PIXELS, MapGeometry.TILE_PIXELS,
            new int[MapGeometry.TILE_PIXELS * MapGeometry.TILE_PIXELS]));

    /** Protocol v1 answer for a pyramid tile with nothing drawn: transparent, as Takaro expects. */
    private static final byte[] EMPTY_PYRAMID_TILE = PngEncoder.encodeRgba(MapPyramid.TILE_SIZE, MapPyramid.TILE_SIZE,
        new int[MapPyramid.TILE_SIZE * MapPyramid.TILE_SIZE]);

    private final ChunkRaster raster;
    private final int capacity;
    private final Map<String, String> cache;
    private long cacheRevision = -1;
    private final Map<String, byte[]> pyramidCache;
    private long pyramidRevision = -1;

    public MapTiles(ChunkRaster raster, int capacity) {
        this.raster = raster;
        this.capacity = capacity;
        this.cache = new LinkedHashMap<>(capacity * 2, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<String, String> eldest) {
                return size() > MapTiles.this.capacity;
            }
        };
        this.pyramidCache = new LinkedHashMap<>(capacity * 2, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<String, byte[]> eldest) {
                return size() > MapTiles.this.capacity;
            }
        };
    }

    /** True when at least one rendered chunk overlaps the tile. */
    public boolean hasData(int zoom, int x, int y) {
        int[] cx = MapGeometry.chunkRange(MapGeometry.tileMinX(zoom, x), zoom);
        int[] cz = MapGeometry.chunkRange(MapGeometry.tileMinZ(zoom, y), zoom);
        return raster.anyIn(cx[0], cx[1], cz[0], cz[1]);
    }

    /** Packed RGBA pixels of a tile, row 0 at the +Z edge. */
    public int[] renderRgba(int zoom, int x, int y) {
        int n = MapGeometry.TILE_PIXELS;
        int[] px = new int[n * n];
        for (int row = 0; row < n; row++) {
            long bz = MapGeometry.pixelBlockZ(zoom, y, row);
            for (int col = 0; col < n; col++) {
                long bx = MapGeometry.pixelBlockX(zoom, x, col);
                px[row * n + col] = raster.pixelAt(bx, bz);
            }
        }
        return px;
    }

    /** Base64 PNG of a tile; {@link #EMPTY_TILE_BASE64} when no explored chunk overlaps it. */
    public String tileBase64(int zoom, int x, int y) {
        String key = zoom + "/" + x + "/" + y;
        long revision = raster.revision();
        synchronized (cache) {
            if (cacheRevision != revision) {
                cache.clear();
                cacheRevision = revision;
            }
            String hit = cache.get(key);
            if (hit != null) {
                return hit;
            }
        }
        if (!hasData(zoom, x, y)) {
            return EMPTY_TILE_BASE64;
        }
        String png = Base64.getEncoder().encodeToString(
            PngEncoder.encodeRgba(MapGeometry.TILE_PIXELS, MapGeometry.TILE_PIXELS, renderRgba(zoom, x, y)));
        synchronized (cache) {
            if (cacheRevision == revision) {
                cache.put(key, png);
            }
        }
        return png;
    }

    /** True when at least one drawn chunk overlaps the v1 pyramid tile. */
    public boolean hasData(MapPyramid.Frame frame, int zoom, int x, int y) {
        int span = frame.span(zoom);
        long x0 = (long) frame.minX() + (long) x * span;
        long z0 = (long) frame.minZ() + (long) y * span;
        return raster.anyIn(MapGeometry.chunkOf(x0), MapGeometry.chunkOf(x0 + span - 1),
            MapGeometry.chunkOf(z0), MapGeometry.chunkOf(z0 + span - 1));
    }

    /** Packed RGBA pixels of a v1 pyramid tile, row 0 at the north edge. */
    public int[] renderPyramidRgba(MapPyramid.Frame frame, int zoom, int x, int y) {
        int n = MapPyramid.TILE_SIZE;
        int[] px = new int[n * n];
        long[] columns = new long[n];
        for (int col = 0; col < n; col++) {
            columns[col] = frame.blockX(zoom, x, col);
        }
        for (int row = 0; row < n; row++) {
            long bz = frame.blockZ(zoom, y, row);
            for (int col = 0; col < n; col++) {
                px[row * n + col] = raster.pixelAt(columns[col], bz);
            }
        }
        return px;
    }

    /**
     * The PNG of one v1 pyramid tile; a transparent tile where nothing is drawn. The caller
     * checks the tile lies inside the frame's grid.
     */
    public byte[] pyramidTilePng(MapPyramid.Frame frame, int zoom, int x, int y) {
        String key = frame + "/" + zoom + "/" + x + "/" + y;
        long revision = raster.revision();
        synchronized (pyramidCache) {
            if (pyramidRevision != revision) {
                pyramidCache.clear();
                pyramidRevision = revision;
            }
            byte[] hit = pyramidCache.get(key);
            if (hit != null) {
                return hit;
            }
        }
        byte[] png = hasData(frame, zoom, x, y)
            ? PngEncoder.encodeRgba(MapPyramid.TILE_SIZE, MapPyramid.TILE_SIZE, renderPyramidRgba(frame, zoom, x, y))
            : EMPTY_PYRAMID_TILE;
        synchronized (pyramidCache) {
            if (pyramidRevision == revision) {
                pyramidCache.put(key, png);
            }
        }
        return png;
    }
}
