package dev.takaro.hytale.map;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;

/**
 * The rendered world map at one pixel per block, stored per chunk as 32x32 packed RGBA
 * ({@code r<<24 | g<<16 | b<<8 | a}, Hytale's own packing). Row {@code z}, column {@code x}
 * inside the chunk, so index {@code z * 32 + x}.
 *
 * <p>Written by the map renderer thread, read by request threads. Each chunk entry is replaced
 * as a whole and never mutated after publication.
 */
public final class ChunkRaster {
    private static final int MAGIC = 0x544B4D50; // "TKMP"
    private static final int FORMAT_VERSION = 2;
    public static final int CELLS = MapGeometry.CHUNK_SIZE * MapGeometry.CHUNK_SIZE;

    /** A rendered chunk: its pixels and when it was rendered (epoch millis). */
    public record Entry(int[] pixels, long renderedAt) {
    }

    private final Map<Long, Entry> chunks = new ConcurrentHashMap<>();
    private final AtomicLong revision = new AtomicLong();
    private volatile boolean dirty;

    /** Same packing as Hytale's {@code ChunkUtil.indexChunk}. */
    public static long key(int chunkX, int chunkZ) {
        return ((long) chunkX << 32) | (chunkZ & 0xFFFFFFFFL);
    }

    public static int keyX(long key) {
        return (int) (key >> 32);
    }

    public static int keyZ(long key) {
        return (int) key;
    }

    public void put(int chunkX, int chunkZ, int[] pixels, long renderedAt) {
        if (pixels.length != CELLS) {
            throw new IllegalArgumentException("chunk raster needs " + CELLS + " pixels, got " + pixels.length);
        }
        chunks.put(key(chunkX, chunkZ), new Entry(pixels, renderedAt));
        revision.incrementAndGet();
        dirty = true;
    }

    /**
     * Drop every chunk whose key is not in {@code keep} (chunks no longer in the save, for example
     * after the world's chunks were wiped). Returns how many were dropped.
     */
    public int retainOnly(java.util.Set<Long> keep) {
        int before = chunks.size();
        chunks.keySet().removeIf(k -> !keep.contains(k));
        int dropped = before - chunks.size();
        if (dropped > 0) {
            revision.incrementAndGet();
            dirty = true;
        }
        return dropped;
    }

    public Entry get(int chunkX, int chunkZ) {
        return chunks.get(key(chunkX, chunkZ));
    }

    public Entry get(long key) {
        return chunks.get(key);
    }

    public int size() {
        return chunks.size();
    }

    /** Increases on every change; tile caches compare it to detect stale entries. */
    public long revision() {
        return revision.get();
    }

    /** Packed RGBA of block (bx, bz), or 0 (transparent) where nothing is rendered. */
    public int pixelAt(long blockX, long blockZ) {
        int cx = MapGeometry.chunkOf(blockX);
        int cz = MapGeometry.chunkOf(blockZ);
        Entry entry = chunks.get(key(cx, cz));
        if (entry == null) {
            return 0;
        }
        int lx = (int) (blockX - (long) cx * MapGeometry.CHUNK_SIZE);
        int lz = (int) (blockZ - (long) cz * MapGeometry.CHUNK_SIZE);
        return entry.pixels[lz * MapGeometry.CHUNK_SIZE + lx];
    }

    /** True when any chunk in the inclusive chunk rectangle is rendered. */
    public boolean anyIn(int minCx, int maxCx, int minCz, int maxCz) {
        long area = (long) (maxCx - minCx + 1) * (maxCz - minCz + 1);
        if (area > chunks.size()) {
            for (Long k : chunks.keySet()) {
                int x = keyX(k);
                int z = keyZ(k);
                if (x >= minCx && x <= maxCx && z >= minCz && z <= maxCz) {
                    return true;
                }
            }
            return false;
        }
        for (int x = minCx; x <= maxCx; x++) {
            for (int z = minCz; z <= maxCz; z++) {
                if (chunks.containsKey(key(x, z))) {
                    return true;
                }
            }
        }
        return false;
    }

    /** Largest |block coordinate| covered by a rendered chunk, or 0 for an empty raster. */
    public int maxAbsBlock() {
        int max = 0;
        for (Long k : chunks.keySet()) {
            int x = keyX(k);
            int z = keyZ(k);
            int bx = Math.max(Math.abs(x * MapGeometry.CHUNK_SIZE), Math.abs((x + 1) * MapGeometry.CHUNK_SIZE));
            int bz = Math.max(Math.abs(z * MapGeometry.CHUNK_SIZE), Math.abs((z + 1) * MapGeometry.CHUNK_SIZE));
            max = Math.max(max, Math.max(bx, bz));
        }
        return max;
    }

    public boolean isDirty() {
        return dirty;
    }

    /**
     * Write the raster atomically (temp file + move). {@code worldId} ties it to one world
     * instance (the world's own UUID, which a re-created world does not share).
     */
    public void save(Path file, String worldId) throws IOException {
        Path tmp = file.resolveSibling(file.getFileName() + ".tmp");
        dirty = false;
        try (OutputStream raw = Files.newOutputStream(tmp);
             DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new GZIPOutputStream(raw)))) {
            Map<Long, Entry> snapshot = Map.copyOf(chunks);
            out.writeInt(MAGIC);
            out.writeInt(FORMAT_VERSION);
            out.writeUTF(worldId);
            out.writeInt(snapshot.size());
            for (Map.Entry<Long, Entry> e : snapshot.entrySet()) {
                out.writeLong(e.getKey());
                out.writeLong(e.getValue().renderedAt());
                for (int p : e.getValue().pixels()) {
                    out.writeInt(p);
                }
            }
        } catch (IOException e) {
            dirty = true;
            throw e;
        }
        Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    /**
     * Load a raster saved by {@link #save}. Returns the number of chunks loaded, or -1 when the
     * file is missing, unreadable, from an older format or belongs to another world (the raster
     * is left empty then).
     *
     * <p>Loaded chunks count as rendered at time 0: the world may have changed while the server
     * was down, so chunks near players are redrawn as soon as they are loaded.
     */
    public int load(Path file, String worldId) {
        if (!Files.isRegularFile(file)) {
            return -1;
        }
        try (InputStream raw = Files.newInputStream(file);
             DataInputStream in = new DataInputStream(new BufferedInputStream(new GZIPInputStream(raw)))) {
            if (in.readInt() != MAGIC || in.readInt() != FORMAT_VERSION || !in.readUTF().equals(worldId)) {
                return -1;
            }
            int count = in.readInt();
            if (count < 0 || count > 4_000_000) {
                return -1;
            }
            Map<Long, Entry> loaded = new java.util.HashMap<>(count * 2);
            for (int i = 0; i < count; i++) {
                long key = in.readLong();
                in.readLong(); // renderedAt from the earlier run
                int[] pixels = new int[CELLS];
                for (int p = 0; p < CELLS; p++) {
                    pixels[p] = in.readInt();
                }
                loaded.put(key, new Entry(pixels, 0));
            }
            chunks.putAll(loaded);
            revision.incrementAndGet();
            return count;
        } catch (IOException | RuntimeException e) {
            return -1;
        }
    }
}
