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

    private final ChunkRaster raster;
    private final int capacity;
    private final Map<String, String> cache;
    private long cacheRevision = -1;

    public MapTiles(ChunkRaster raster, int capacity) {
        this.raster = raster;
        this.capacity = capacity;
        this.cache = new LinkedHashMap<>(capacity * 2, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<String, String> eldest) {
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
}
