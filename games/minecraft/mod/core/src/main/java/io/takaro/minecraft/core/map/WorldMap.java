package io.takaro.minecraft.core.map;

import java.io.IOException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;

/**
 * A top-down map of each dimension, rendered from the region files the server saves, served as
 * Takaro protocol-v1 map tiles. Nothing here touches the game thread: region files are read
 * from disk on one low-priority render thread, so the map shows what the server last saved
 * (autosave, or {@code save-all}).
 *
 * <p>Tiles follow Takaro's pyramid: the frame is a square anchored at the top-left (north-west)
 * corner of the declared bounds, and zoom {@code z} splits it into {@code 2^z x 2^z} tiles of
 * {@link #TILE_SIZE} pixels. The frame covers whole regions and grows in powers of two as the
 * world is explored; at the deepest zoom one block is two pixels.
 */
public final class WorldMap {

    public static final int TILE_SIZE = 256;
    private static final int MIN_SIDE_REGIONS = 4;
    private static final int RASTER_CACHE = 64;
    private static final int TILE_CACHE = 512;

    /** A dimension and the folder holding its {@code r.x.z.mca} files. */
    public record Dimension(String id, String name, Path regionDir, boolean ceiling) {}

    /** The square the tile pyramid covers, in blocks: x grows east (right), z grows south (down). */
    public record Frame(int minX, int minZ, int size, int maxZoom) {
        public int maxX() { return minX + size; }
        public int maxZ() { return minZ + size; }
    }

    /** The tile is not rendered yet; it keeps rendering and a later request gets it. */
    public static final class NotReadyException extends Exception {
        public NotReadyException(String message) { super(message); }
    }

    private record Raster(int[] argb, long modified) {}
    private record Tile(byte[] png, long fingerprint) {}

    private final List<Dimension> dimensions;
    private final Map<String, Dimension> byId = new HashMap<>();
    private final RegionRenderer renderer;
    private final Consumer<String> log;
    private final Map<String, Frame> frames = new ConcurrentHashMap<>();
    private final Map<String, Future<byte[]>> inflight = new ConcurrentHashMap<>();
    // Tiles are put together on one thread from cached region drawings; redrawing a region the
    // server saved again takes seconds and runs on another, so it never holds up a tile.
    private final ExecutorService renderThread = daemonThread("takaro-map-render");
    private final ExecutorService redrawThread = daemonThread("takaro-map-redraw");

    private final Map<String, Raster> rasters = java.util.Collections.synchronizedMap(lru(RASTER_CACHE));
    private final java.util.Set<String> refreshing = ConcurrentHashMap.newKeySet();
    // Only the render thread touches this.
    private final Map<String, Tile> tiles = lru(TILE_CACHE);

    private static ExecutorService daemonThread(String name) {
        return Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, name);
            t.setDaemon(true);
            t.setPriority(Thread.MIN_PRIORITY);
            return t;
        });
    }

    public WorldMap(List<Dimension> dimensions, RegionRenderer.BlockColors colors, Consumer<String> log) {
        this.dimensions = List.copyOf(dimensions);
        for (Dimension d : this.dimensions) byId.put(d.id(), d);
        this.renderer = new RegionRenderer(colors);
        this.log = log;
        refreshFrames();
    }

    /**
     * The region folder of a dimension, for both save layouts: Minecraft 26.x keeps every
     * dimension under {@code dimensions/<namespace>/<path>/region}; earlier versions keep the
     * overworld in {@code region}, the Nether in {@code DIM-1/region} and the End in
     * {@code DIM1/region}. The first that exists wins; a dimension not saved yet gets the new layout.
     */
    public static Path regionDir(Path worldRoot, String namespace, String path) {
        Path modern = worldRoot.resolve("dimensions").resolve(namespace).resolve(path).resolve("region");
        Path legacy = !"minecraft".equals(namespace) ? null : switch (path) {
            case "overworld" -> worldRoot.resolve("region");
            case "the_nether" -> worldRoot.resolve("DIM-1").resolve("region");
            case "the_end" -> worldRoot.resolve("DIM1").resolve("region");
            default -> null;
        };
        if (Files.isDirectory(modern)) return modern;
        if (legacy != null && Files.isDirectory(legacy)) return legacy;
        return modern;
    }

    public List<Dimension> dimensions() {
        return dimensions;
    }

    public Frame frame(String dimensionId) {
        return frames.get(dimensionId);
    }

    /** Re-reads which regions exist; true when a frame changed and the map must be re-declared. */
    public boolean refreshFrames() {
        boolean changed = false;
        for (Dimension d : dimensions) {
            Frame next = computeFrame(d.regionDir(), frames.get(d.id()));
            Frame previous = frames.put(d.id(), next);
            changed |= !next.equals(previous);
        }
        return changed;
    }

    static Frame computeFrame(Path regionDir) {
        return computeFrame(regionDir, null);
    }

    /**
     * The frame for the saved regions. A {@code previous} frame that still holds every region
     * is kept, so tile coordinates (and every cache keyed on them) only change when the world
     * outgrows the map.
     */
    static Frame computeFrame(Path regionDir, Frame previous) {
        int minRX = Integer.MAX_VALUE, minRZ = Integer.MAX_VALUE;
        int maxRX = Integer.MIN_VALUE, maxRZ = Integer.MIN_VALUE;
        if (regionDir != null && Files.isDirectory(regionDir)) {
            try (DirectoryStream<Path> files = Files.newDirectoryStream(regionDir, "r.*.mca")) {
                for (Path f : files) {
                    int[] rc = RegionFile.coordinates(f.getFileName().toString());
                    // A header-only file (8 KiB) holds no chunks yet.
                    if (rc == null || Files.size(f) <= 8192) continue;
                    minRX = Math.min(minRX, rc[0]);
                    maxRX = Math.max(maxRX, rc[0]);
                    minRZ = Math.min(minRZ, rc[1]);
                    maxRZ = Math.max(maxRZ, rc[1]);
                }
            } catch (IOException e) {
                // fall through to the default frame
            }
        }
        if (minRX == Integer.MAX_VALUE) {
            minRX = minRZ = -MIN_SIDE_REGIONS / 2;
            maxRX = maxRZ = MIN_SIDE_REGIONS / 2 - 1;
        }
        if (previous != null) {
            int pMinRX = Math.floorDiv(previous.minX(), RegionFile.BLOCKS);
            int pMinRZ = Math.floorDiv(previous.minZ(), RegionFile.BLOCKS);
            int pSide = previous.size() / RegionFile.BLOCKS;
            if (minRX >= pMinRX && minRZ >= pMinRZ && maxRX < pMinRX + pSide && maxRZ < pMinRZ + pSide) {
                return previous;
            }
        }
        int extent = Math.max(maxRX - minRX + 1, maxRZ - minRZ + 1);
        int side = MIN_SIDE_REGIONS;
        while (side < extent) side *= 2;
        // Centre the explored area in the square, still on whole regions.
        int ox = minRX - (side - (maxRX - minRX + 1)) / 2;
        int oz = minRZ - (side - (maxRZ - minRZ + 1)) / 2;
        int blocks = side * RegionFile.BLOCKS;
        // One block per pixel where a tile spans TILE_SIZE blocks; one zoom deeper gives two.
        int maxZoom = Integer.numberOfTrailingZeros(blocks / TILE_SIZE) + 1;
        return new Frame(ox * RegionFile.BLOCKS, oz * RegionFile.BLOCKS, blocks, maxZoom);
    }

    /**
     * The PNG of one tile. Waits up to {@code waitMillis} for the render; past that it throws
     * {@link NotReadyException} and the render finishes in the background for the next request.
     */
    public byte[] tile(String dimensionId, int zoom, int x, int y, long waitMillis)
            throws NotReadyException {
        Dimension d = byId.get(dimensionId);
        if (d == null) throw new IllegalArgumentException("unknown dimension " + dimensionId);
        Frame frame = frames.get(dimensionId);
        if (zoom < 0 || zoom > frame.maxZoom()) {
            throw new IllegalArgumentException("zoom " + zoom + " outside 0.." + frame.maxZoom());
        }
        int n = 1 << zoom;
        if (x < 0 || y < 0 || x >= n || y >= n) {
            throw new IllegalArgumentException("tile " + x + "," + y + " outside the " + n + "x" + n + " grid");
        }
        String key = dimensionId + "/" + zoom + "/" + x + "/" + y + "@" + frame;
        Future<byte[]> future = inflight.computeIfAbsent(key, k -> renderThread.submit(() -> {
            try {
                return renderTile(d, frame, zoom, x, y, k);
            } finally {
                inflight.remove(k);
            }
        }));
        try {
            return future.get(waitMillis, TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            throw new NotReadyException("map tile is still rendering");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new NotReadyException("interrupted");
        } catch (ExecutionException e) {
            throw new IllegalStateException("map render failed: " + e.getCause(), e.getCause());
        }
    }

    /** Renders the whole of every dimension once in the background, so first requests are fast. */
    public void warm() {
        for (Dimension d : dimensions) {
            Frame frame = frames.get(d.id());
            renderThread.submit(() -> {
                long start = System.nanoTime();
                try {
                    renderTile(d, frame, 0, 0, 0, d.id() + "/0/0/0@" + frame);
                    log.accept("Map " + d.id() + " rendered (" + frame.size() + " blocks square) in "
                            + (System.nanoTime() - start) / 1_000_000 + " ms");
                } catch (RuntimeException e) {
                    log.accept("Map " + d.id() + " render failed: " + e);
                }
                return null;
            });
        }
    }

    public void close() {
        renderThread.shutdownNow();
        redrawThread.shutdownNow();
    }

    // --- render thread (tiles) and redraw thread (rasters) ---

    private byte[] renderTile(Dimension d, Frame frame, int zoom, int tx, int ty, String key) {
        int span = frame.size() >> zoom; // blocks across this tile
        int x0 = frame.minX() + tx * span;
        int z0 = frame.minZ() + ty * span;
        int rx0 = Math.floorDiv(x0, RegionFile.BLOCKS), rx1 = Math.floorDiv(x0 + span - 1, RegionFile.BLOCKS);
        int rz0 = Math.floorDiv(z0, RegionFile.BLOCKS), rz1 = Math.floorDiv(z0 + span - 1, RegionFile.BLOCKS);

        // Keyed on the rasters the tile is drawn from: a stale raster still being refreshed
        // gives the old fingerprint, so the tile is redrawn once the fresh raster lands.
        long fingerprint = 17;
        for (int rz = rz0; rz <= rz1; rz++) {
            for (int rx = rx0; rx <= rx1; rx++) {
                Raster cachedRaster = refreshIfStale(d, rx, rz);
                fingerprint = fingerprint * 31 + (cachedRaster != null ? cachedRaster.modified() : modified(regionPath(d, rx, rz)));
            }
        }
        Tile cached = tiles.get(key);
        if (cached != null && cached.fingerprint() == fingerprint) return cached.png();

        int[] out = new int[TILE_SIZE * TILE_SIZE];
        Raster raster = null;
        int rasterRX = Integer.MIN_VALUE, rasterRZ = Integer.MIN_VALUE;
        for (int py = 0; py < TILE_SIZE; py++) {
            int bz = z0 + (int) ((long) py * span / TILE_SIZE);
            int rz = Math.floorDiv(bz, RegionFile.BLOCKS);
            for (int px = 0; px < TILE_SIZE; px++) {
                int bx = x0 + (int) ((long) px * span / TILE_SIZE);
                int rx = Math.floorDiv(bx, RegionFile.BLOCKS);
                if (rx != rasterRX || rz != rasterRZ) {
                    raster = raster(d, rx, rz);
                    rasterRX = rx;
                    rasterRZ = rz;
                }
                if (raster != null) {
                    out[py * TILE_SIZE + px] = raster.argb()[Math.floorMod(bz, RegionFile.BLOCKS) * RegionFile.BLOCKS
                            + Math.floorMod(bx, RegionFile.BLOCKS)];
                }
            }
        }
        byte[] png = PngWriter.write(out, TILE_SIZE, TILE_SIZE);
        tiles.put(key, new Tile(png, fingerprint));
        return png;
    }

    /**
     * The region's pixels. A region the server saved again since it was drawn is served from
     * the old drawing at once and redrawn in the background: a redraw takes seconds, and Takaro
     * shows a blank placeholder for any tile that is not ready in time.
     */
    private Raster raster(Dimension d, int rx, int rz) {
        Raster cached = refreshIfStale(d, rx, rz);
        return cached != null ? cached : drawRaster(d, rx, rz);
    }

    /**
     * The cached drawing of a region, queueing a redraw when the server saved it since. A region
     * whose file is gone (trimmed or reset area) is forgotten at once, so every tile drawn from it
     * changes fingerprint and is drawn again without it.
     */
    private Raster refreshIfStale(Dimension d, int rx, int rz) {
        String key = rasterKey(d, rx, rz);
        Raster cached = rasters.get(key);
        if (cached == null) return null;
        long modified = modified(regionPath(d, rx, rz));
        if (modified == 0) {
            rasters.remove(key);
            return null;
        }
        if (modified != cached.modified() && refreshing.add(key)) {
            redrawThread.submit(() -> {
                try {
                    drawRaster(d, rx, rz);
                } finally {
                    refreshing.remove(key);
                }
            });
        }
        return cached;
    }

    private Raster drawRaster(Dimension d, int rx, int rz) {
        Path path = regionPath(d, rx, rz);
        long modified = modified(path);
        if (modified == 0) return null;
        try {
            Raster fresh = new Raster(renderer.render(RegionFile.read(path), d.ceiling()), modified);
            rasters.put(rasterKey(d, rx, rz), fresh);
            return fresh;
        } catch (IOException e) {
            return rasters.get(rasterKey(d, rx, rz));
        }
    }

    private static String rasterKey(Dimension d, int rx, int rz) {
        return d.id() + "/" + rx + "/" + rz;
    }

    private static Path regionPath(Dimension d, int rx, int rz) {
        return d.regionDir().resolve("r." + rx + "." + rz + ".mca");
    }

    private static long modified(Path path) {
        try {
            BasicFileAttributes a = Files.readAttributes(path, BasicFileAttributes.class);
            return Objects.hash(a.lastModifiedTime().toMillis(), a.size());
        } catch (IOException e) {
            return 0;
        }
    }

    private static <K, V> Map<K, V> lru(int capacity) {
        return new LinkedHashMap<>(16, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<K, V> eldest) {
                return size() > capacity;
            }
        };
    }

    /** The Takaro {@code map} capability parameters for these dimensions. */
    public List<Map<String, Object>> declaration() {
        List<Map<String, Object>> out = new ArrayList<>();
        for (Dimension d : dimensions) {
            Frame f = frames.get(d.id());
            Map<String, Object> dim = new LinkedHashMap<>();
            dim.put("id", d.id());
            dim.put("name", d.name());
            dim.put("axes", Map.of(
                    "horizontal", Map.of("field", "x", "direction", "right"),
                    "vertical", Map.of("field", "z", "direction", "down")));
            dim.put("bounds", Map.of(
                    "horizontal", Map.of("min", f.minX(), "max", f.maxX()),
                    "vertical", Map.of("min", f.minZ(), "max", f.maxZ())));
            dim.put("source", Map.of("type", "tiled", "tileSize", TILE_SIZE, "minZoom", 0, "maxZoom", f.maxZoom()));
            out.add(dim);
        }
        return out;
    }
}
