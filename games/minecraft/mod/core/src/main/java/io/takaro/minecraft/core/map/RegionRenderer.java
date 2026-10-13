package io.takaro.minecraft.core.map;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Renders one region (512x512 blocks) top-down the way a vanilla filled map does: the first
 * block from the top that has a map colour, shaded brighter or darker against its northern
 * neighbour, and water shaded by depth. Pixels of chunks that were never generated stay
 * transparent (0).
 */
public final class RegionRenderer {

    /** Block colours: 0xRRGGBB, or {@link #NONE} for blocks a map shows through (air, glass). */
    public interface BlockColors {
        int NONE = -1;

        int color(String blockName);

        /** The colour that marks water, for depth shading; vanilla MapColor.WATER. */
        default int waterColor() {
            return 0x4040FF;
        }
    }

    private static final int SIZE = RegionFile.BLOCKS;
    private static final int LOW = 180;
    private static final int NORMAL = 220;
    private static final int HIGH = 255;
    // Below this many blocks of headroom a ceiling dimension (the Nether) is scanned from here.
    private static final int CEILING_SCAN_TOP = 127;

    private final BlockColors colors;
    private final Map<String, Integer> colorCache = new HashMap<>();

    public RegionRenderer(BlockColors colors) {
        this.colors = colors;
    }

    /** ARGB pixels, row-major, z rows (north at the top) by x columns. */
    public int[] render(RegionFile region, boolean ceiling) {
        int[] base = new int[SIZE * SIZE];
        int[] height = new int[SIZE * SIZE];
        int[] depth = new int[SIZE * SIZE];
        boolean[] present = new boolean[SIZE * SIZE];
        for (int cz = 0; cz < RegionFile.CHUNKS; cz++) {
            for (int cx = 0; cx < RegionFile.CHUNKS; cx++) {
                Map<String, Object> chunk = region.chunk(cx, cz);
                if (chunk != null) {
                    renderChunk(chunk, cx * 16, cz * 16, ceiling, base, height, depth, present);
                }
            }
        }
        return shade(base, height, depth, present);
    }

    private int[] shade(int[] base, int[] height, int[] depth, boolean[] present) {
        int water = colors.waterColor();
        int[] argb = new int[SIZE * SIZE];
        for (int z = 0; z < SIZE; z++) {
            for (int x = 0; x < SIZE; x++) {
                int i = z * SIZE + x;
                if (!present[i]) continue;
                int brightness;
                int checker = (x + z) & 1;
                if (base[i] == water) {
                    double d = depth[i] * 0.1 + checker * 0.2;
                    brightness = d < 0.5 ? HIGH : d > 0.9 ? LOW : NORMAL;
                } else {
                    int north = z > 0 && present[i - SIZE] ? height[i - SIZE] : height[i];
                    double d = (height[i] - north) * 4.0 / 5.0 + (checker - 0.5) * 0.4;
                    brightness = d > 0.6 ? HIGH : d < -0.6 ? LOW : NORMAL;
                }
                int c = base[i];
                int r = ((c >> 16) & 0xff) * brightness / 255;
                int g = ((c >> 8) & 0xff) * brightness / 255;
                int b = (c & 0xff) * brightness / 255;
                argb[i] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
        return argb;
    }

    private void renderChunk(Map<String, Object> chunk, int ox, int oz, boolean ceiling,
                             int[] base, int[] height, int[] depth, boolean[] present) {
        String status = Nbt.string(chunk, "Status");
        if (status != null && !status.equals("minecraft:full") && !status.equals("full")) {
            return; // still generating: its blocks are not final
        }
        Map<Integer, Section> sections = new HashMap<>();
        int minSection = Integer.MAX_VALUE;
        int maxSection = Integer.MIN_VALUE;
        for (Object o : Nbt.list(chunk, "sections")) {
            if (!(o instanceof Map)) continue;
            @SuppressWarnings("unchecked")
            Map<String, Object> s = (Map<String, Object>) o;
            Integer y = Nbt.integer(s, "Y");
            Section section = Section.of(Nbt.compound(s, "block_states"), this::colorOf);
            if (y == null || section == null) continue;
            sections.put(y, section);
            minSection = Math.min(minSection, y);
            maxSection = Math.max(maxSection, y);
        }
        if (sections.isEmpty()) return;
        Integer yPos = Nbt.integer(chunk, "yPos");
        int minY = (yPos != null ? yPos : minSection) * 16;
        int maxY = maxSection * 16 + 15;
        long[] surface = Nbt.longs(Nbt.compound(chunk, "Heightmaps"), "WORLD_SURFACE");
        int water = colors.waterColor();

        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                int top;
                if (ceiling) {
                    top = Math.min(maxY, minY + CEILING_SCAN_TOP);
                    // Skip the roof: go down to the first block the map shows through.
                    while (top > minY && colorAt(sections, x, top, z) != BlockColors.NONE) top--;
                } else if (surface != null) {
                    top = minY + heightmapValue(surface, z * 16 + x) - 1;
                } else {
                    top = maxY;
                }
                int y = top;
                int color = BlockColors.NONE;
                while (y >= minY) {
                    color = colorAt(sections, x, y, z);
                    if (color != BlockColors.NONE) break;
                    y--;
                }
                if (color == BlockColors.NONE) continue;
                int i = (oz + z) * SIZE + ox + x;
                int waterDepth = 0;
                if (color == water) {
                    int yy = y - 1;
                    while (yy >= minY && waterDepth < 16 && colorAt(sections, x, yy, z) == water) {
                        waterDepth++;
                        yy--;
                    }
                }
                base[i] = color;
                height[i] = y;
                depth[i] = waterDepth;
                present[i] = true;
            }
        }
    }

    private static int colorAt(Map<Integer, Section> sections, int x, int y, int z) {
        Section s = sections.get(Math.floorDiv(y, 16));
        return s == null ? BlockColors.NONE : s.color(x, Math.floorMod(y, 16), z);
    }

    /** Heightmaps pack 256 values per chunk without spanning longs; the bit width follows from the length. */
    static int heightmapValue(long[] packed, int index) {
        int perLong = (256 + packed.length - 1) / packed.length;
        int bits = 64 / perLong;
        long word = packed[index / perLong];
        return (int) ((word >>> ((index % perLong) * bits)) & ((1L << bits) - 1));
    }

    private int colorOf(String name) {
        return colorCache.computeIfAbsent(name, colors::color);
    }

    /** One 16x16x16 section with its palette already turned into colours. */
    static final class Section {
        private final int[] paletteColors;
        private final long[] data;
        private final int bits;
        private final int perLong;

        private Section(int[] paletteColors, long[] data) {
            this.paletteColors = paletteColors;
            this.data = data;
            int b = 4;
            while ((1 << b) < paletteColors.length) b++;
            this.bits = b;
            this.perLong = 64 / b;
        }

        static Section of(Map<String, Object> blockStates, java.util.function.ToIntFunction<String> color) {
            if (blockStates == null) return null;
            List<Object> palette = Nbt.list(blockStates, "palette");
            if (palette.isEmpty()) return null;
            int[] colors = new int[palette.size()];
            for (int i = 0; i < colors.length; i++) {
                Object entry = palette.get(i);
                @SuppressWarnings("unchecked")
                String name = entry instanceof Map ? Nbt.string((Map<String, Object>) entry, "Name") : null;
                colors[i] = name == null ? BlockColors.NONE : color.applyAsInt(name);
            }
            return new Section(colors, Nbt.longs(blockStates, "data"));
        }

        int color(int x, int y, int z) {
            if (paletteColors.length == 1 || data == null) return paletteColors[0];
            int index = (y * 16 + z) * 16 + x;
            int word = index / perLong;
            if (word >= data.length) return BlockColors.NONE;
            int value = (int) ((data[word] >>> ((index % perLong) * bits)) & ((1L << bits) - 1));
            return value < paletteColors.length ? paletteColors[value] : BlockColors.NONE;
        }
    }
}
