package dev.takaro.hytale.map;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Set;

/**
 * Decides which chunks the map renderer draws next.
 *
 * <p>Hytale's chunk image builder also reads the eight neighbouring chunks for shading, and it
 * fetches them with flags that <em>generate</em> a chunk that does not exist yet. Drawing the
 * border of the explored area would therefore create new terrain, which is saved, drawn, and
 * grows the world without anyone exploring it. The planner only picks chunks whose eight
 * neighbours already exist, so rendering never generates world.
 *
 * <p>Loading a saved chunk that nobody is near costs the world thread 40-500 ms of post-load
 * work (measured on 0.6.8). So while players are online only chunks whose whole 3x3 block is
 * already loaded are drawn; saved but unloaded chunks are drawn only while the world is empty.
 */
public final class RenderPlanner {
    private RenderPlanner() {
    }

    /** True when the chunk and its eight neighbours are all in {@code existing}. */
    public static boolean isInterior(Set<Long> existing, int cx, int cz) {
        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (!existing.contains(ChunkRaster.key(cx + dx, cz + dz))) {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * Chunks to render, nearest the origin first: interior chunks never rendered, then interior
     * chunks in {@code loaded} (near players, so they may have changed) whose image is older than
     * {@code refreshAfterMillis}. Unless {@code worldIdle}, every chunk returned has its whole 3x3
     * block in {@code loaded}, so drawing it loads nothing.
     */
    public static List<Long> plan(Set<Long> existing, Set<Long> loaded, ChunkRaster raster,
                                  long nowMillis, long refreshAfterMillis, int limit, boolean worldIdle) {
        List<Long> missing = new ArrayList<>();
        for (Long key : existing) {
            int cx = ChunkRaster.keyX(key);
            int cz = ChunkRaster.keyZ(key);
            if (raster.get(key) == null && isInterior(existing, cx, cz)
                && (worldIdle || isInterior(loaded, cx, cz))) {
                missing.add(key);
            }
        }
        missing.sort(Comparator.comparingLong(RenderPlanner::distanceSquared));

        List<Long> out = new ArrayList<>(Math.min(limit, missing.size()));
        for (Long key : missing) {
            if (out.size() >= limit) {
                return out;
            }
            out.add(key);
        }
        for (Long key : loaded) {
            if (out.size() >= limit) {
                break;
            }
            ChunkRaster.Entry entry = raster.get(key);
            if (entry != null && nowMillis - entry.renderedAt() >= refreshAfterMillis
                && isInterior(loaded, ChunkRaster.keyX(key), ChunkRaster.keyZ(key))) {
                out.add(key);
            }
        }
        return out;
    }

    private static long distanceSquared(long key) {
        long x = ChunkRaster.keyX(key);
        long z = ChunkRaster.keyZ(key);
        return x * x + z * z;
    }
}
