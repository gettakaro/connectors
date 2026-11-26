package dev.takaro.hytale.map;

import com.hypixel.hytale.protocol.packets.worldmap.MapImage;
import com.hypixel.hytale.server.core.universe.Universe;
import com.hypixel.hytale.server.core.universe.world.World;
import com.hypixel.hytale.server.core.universe.world.map.WorldMap;
import com.hypixel.hytale.server.core.universe.world.worldmap.IWorldMap;
import com.hypixel.hytale.server.core.universe.world.worldmap.WorldMapManager;
import dev.takaro.hytale.TakaroPlugin;
import it.unimi.dsi.fastutil.longs.LongList;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import it.unimi.dsi.fastutil.longs.LongSet;

import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.logging.Level;

/**
 * Serves the default world's map to Takaro from the server's own world data.
 *
 * <p>The pictures come from the world's map generator, the same one that draws the in-game map
 * for clients ({@code WorldMapManager.getGenerator()}), so tiles match what players see. A
 * background thread draws every explored chunk once into a {@link ChunkRaster} (persisted next
 * to the config, so a restart does not redraw), then keeps chunks near players fresh. Tile
 * requests only read the raster and never touch the game.
 *
 * <p>Game-thread cost: the generator reads each chunk's components on the world thread (the
 * engine requires that) and rasterises off it. Batches are small and paced, and only chunks
 * that already exist are drawn (see {@link RenderPlanner}), so no world is generated. While
 * players are online only already-loaded chunks are drawn; loading the rest of the saved world
 * (40-500 ms of world-thread work per chunk) waits until the world is empty.
 */
public final class WorldMapService {
    private static final int BATCH = 16;
    private static final long BATCH_PAUSE_MS = 100;
    private static final long IDLE_PAUSE_MS = 5_000;
    private static final long SCAN_EVERY_MS = 60_000;
    private static final long REFRESH_AFTER_MS = 5 * 60_000;
    private static final long SAVE_EVERY_MS = 5 * 60_000;
    private static final long FAILED_RETRY_MS = 10 * 60_000;
    private static final long BATCH_TIMEOUT_S = 30;

    private final TakaroPlugin plugin;
    private final Path dataDir;
    private final ChunkRaster raster = new ChunkRaster();
    private final MapTiles tiles = new MapTiles(raster, 512);
    private final Map<Long, Long> failedAt = new HashMap<>();
    private volatile boolean running;
    private volatile boolean ready;
    private volatile String status = "starting";
    private Thread thread;

    public WorldMapService(TakaroPlugin plugin, Path dataDir) {
        this.plugin = plugin;
        this.dataDir = dataDir;
    }

    public void start() {
        running = true;
        thread = new Thread(this::run, "Takaro-Map");
        thread.setDaemon(true);
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
    }

    public void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
            try {
                thread.join(5_000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    public boolean isReady() {
        return ready;
    }

    public String status() {
        return status;
    }

    public ChunkRaster raster() {
        return raster;
    }

    public MapTiles tiles() {
        return tiles;
    }

    private void log(Level level, String message) {
        plugin.getLogger().at(level).log("[Map] " + message);
    }

    private void run() {
        World world = null;
        IWorldMap generator = null;
        while (running && (world == null || generator == null)) {
            try {
                Universe universe = Universe.get();
                world = universe != null ? universe.getDefaultWorld() : null;
                WorldMapManager manager = world != null ? world.getWorldMapManager() : null;
                generator = manager != null ? manager.getGenerator() : null;
            } catch (Exception e) {
                world = null;
            }
            if (world == null || generator == null) {
                status = world == null ? "waiting for the default world" : "world map is disabled on this world";
                if (!sleep(IDLE_PAUSE_MS)) {
                    return;
                }
            }
        }
        if (!running) {
            return;
        }

        // The world's UUID is random per world creation and kept in its config.json, so a world
        // re-created with the same name and seed does not reuse the old drawings.
        String worldKey = world.getWorldConfig().getUuid() + ":" + world.getWorldConfig().getSeed();
        Path file = dataDir.resolve("worldmap-" + world.getName().replaceAll("[^A-Za-z0-9_.-]", "_") + ".bin");
        int cached = raster.load(file, worldKey);
        if (cached >= 0) {
            log(Level.INFO, "Loaded " + cached + " map chunks from " + file.getFileName());
        }
        int imageSize = Math.max(1, (int) Math.floor(32.0f * world.getWorldMapManager().getWorldMapSettings().getImageScale()));
        ready = true;
        status = "ready";
        log(Level.INFO, "World map ready for '" + world.getName() + "' (" + generator.getClass().getSimpleName()
            + ", " + imageSize + " px per chunk)");

        Set<Long> existing = new HashSet<>();
        long lastScan = 0;
        long lastSave = System.currentTimeMillis();
        int drawnSinceLog = 0;
        while (running) {
            try {
                long now = System.currentTimeMillis();
                IWorldMap current = world.getWorldMapManager().getGenerator();
                if (current == null) {
                    status = "world map is disabled on this world";
                    if (!sleep(IDLE_PAUSE_MS)) {
                        break;
                    }
                    continue;
                }
                generator = current;
                status = "ready";

                boolean scanned = false;
                if (now - lastScan >= SCAN_EVERY_MS) {
                    Set<Long> saved = scanExisting(world);
                    if (saved != null) {
                        existing = saved;
                        scanned = true;
                    }
                    lastScan = now;
                }
                LongSet loadedNow = world.getChunkStore().getChunkIndexes();
                Set<Long> loaded = new HashSet<>(loadedNow.size() * 2);
                for (long key : loadedNow) {
                    loaded.add(key);
                    existing.add(key);
                }
                if (scanned) {
                    // Chunks deleted from the save (a chunk wipe keeps the world's UUID) must not
                    // keep their old drawing next to freshly generated ones.
                    int dropped = raster.retainOnly(existing);
                    if (dropped > 0) {
                        log(Level.INFO, "Dropped " + dropped + " map chunks that are no longer in the save");
                    }
                }

                // Saved chunks far from players are only loaded for drawing while nobody plays.
                boolean idle = world.getPlayerCount() == 0;
                List<Long> plan = RenderPlanner.plan(existing, loaded, raster, now, REFRESH_AFTER_MS, BATCH * 4, idle);
                plan.removeIf(key -> {
                    Long failed = failedAt.get(key);
                    return failed != null && now - failed < FAILED_RETRY_MS;
                });
                if (plan.size() > BATCH) {
                    plan = plan.subList(0, BATCH);
                }

                if (plan.isEmpty()) {
                    if (drawnSinceLog > 0) {
                        log(Level.INFO, "Map up to date: " + raster.size() + " chunks drawn");
                        drawnSinceLog = 0;
                    }
                    if (raster.isDirty() && now - lastSave >= SAVE_EVERY_MS) {
                        save(file, worldKey);
                        lastSave = now;
                    }
                    if (!sleep(IDLE_PAUSE_MS)) {
                        break;
                    }
                    continue;
                }

                drawnSinceLog += draw(world, generator, imageSize, plan, now);
                if (drawnSinceLog >= 500) {
                    log(Level.INFO, "Map drawing: " + raster.size() + " chunks drawn so far");
                    drawnSinceLog = 0;
                }
                if (raster.isDirty() && now - lastSave >= SAVE_EVERY_MS) {
                    save(file, worldKey);
                    lastSave = now;
                }
                if (!sleep(BATCH_PAUSE_MS)) {
                    break;
                }
            } catch (Exception e) {
                log(Level.WARNING, "Map renderer error: " + e);
                if (!sleep(IDLE_PAUSE_MS)) {
                    break;
                }
            }
        }
        if (raster.isDirty()) {
            save(file, worldKey);
        }
    }

    /** Keys of every chunk in the world's save, or null when the save cannot be listed. */
    private Set<Long> scanExisting(World world) {
        Set<Long> keys = new HashSet<>();
        try {
            LongList saved = world.getChunkStore().getLoader().getIndexes();
            for (int i = 0; i < saved.size(); i++) {
                keys.add(saved.getLong(i));
            }
        } catch (Exception e) {
            log(Level.WARNING, "Could not list saved chunks: " + e);
            return null;
        }
        return keys;
    }

    private int draw(World world, IWorldMap generator, int imageSize, List<Long> batch, long now) throws Exception {
        LongSet request = new LongOpenHashSet(batch.size());
        for (Long key : batch) {
            request.add(key.longValue());
        }
        WorldMap result = generator.generate(world, imageSize, imageSize, request).get(BATCH_TIMEOUT_S, TimeUnit.SECONDS);
        int drawn = 0;
        for (Long key : batch) {
            MapImage image = result != null ? result.getChunks().get(key.longValue()) : null;
            int[] pixels = image == null ? null : MapImageDecoder.toChunkPixels(
                image.width, image.height, image.palette, image.bitsPerIndex, image.packedIndices);
            if (pixels == null) {
                failedAt.put(key, now);
                continue;
            }
            failedAt.remove(key);
            raster.put(ChunkRaster.keyX(key), ChunkRaster.keyZ(key), pixels, now);
            drawn++;
        }
        return drawn;
    }

    private void save(Path file, String worldKey) {
        try {
            raster.save(file, worldKey);
        } catch (Exception e) {
            log(Level.WARNING, "Could not save the map cache: " + e);
        }
    }

    private boolean sleep(long ms) {
        try {
            Thread.sleep(ms);
            return running;
        } catch (InterruptedException e) {
            return false;
        }
    }
}
