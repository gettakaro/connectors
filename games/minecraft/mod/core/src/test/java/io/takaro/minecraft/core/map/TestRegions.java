package io.takaro.minecraft.core.map;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.IntBinaryOperator;
import java.util.zip.DeflaterOutputStream;

/** Builds real Anvil region files (zlib chunks, 1.18+ layout) for tests. */
public final class TestRegions {

    public static final int STONE = 0x707070;
    public static final int GRASS = 0x7FB238;
    public static final int WATER = 0x4040FF;

    private TestRegions() {}

    public static RegionRenderer.BlockColors colors() {
        return name -> switch (name) {
            case "minecraft:stone" -> STONE;
            case "minecraft:grass_block" -> GRASS;
            case "minecraft:water" -> WATER;
            default -> RegionRenderer.BlockColors.NONE; // air, glass
        };
    }

    /**
     * One chunk with sections from y=-64 to 319 whose surface is {@code block} at the height
     * {@code surfaceY.applyAsInt(x, z)}, stone below and air above. {@code glassAt} puts one
     * glass block on top of (0,0) so the renderer has to look through it.
     */
    public static Map<String, Object> chunk(int chunkX, int chunkZ, String block, IntBinaryOperator surfaceY,
                                            boolean glassAt00) {
        int minY = -64;
        List<Object> sections = new ArrayList<>();
        int[] heights = new int[256];
        for (int sy = -4; sy < 20; sy++) {
            List<Object> palette = new ArrayList<>();
            palette.add(Map.of("Name", "minecraft:air"));
            palette.add(Map.of("Name", "minecraft:stone"));
            palette.add(Map.of("Name", block));
            palette.add(Map.of("Name", "minecraft:glass"));
            long[] data = new long[4096 * 4 / 64];
            boolean any = false;
            for (int y = 0; y < 16; y++) {
                for (int z = 0; z < 16; z++) {
                    for (int x = 0; x < 16; x++) {
                        int wy = sy * 16 + y;
                        int top = surfaceY.applyAsInt(x, z);
                        int v = wy < top ? 1 : wy == top ? 2 : (glassAt00 && x == 0 && z == 0 && wy == top + 1) ? 3 : 0;
                        if (v != 0) any = true;
                        int i = (y * 16 + z) * 16 + x;
                        data[i / 16] |= (long) v << ((i % 16) * 4);
                    }
                }
            }
            Map<String, Object> states = new LinkedHashMap<>();
            states.put("palette", any ? palette : List.of(Map.of("Name", "minecraft:air")));
            if (any) states.put("data", data);
            Map<String, Object> section = new LinkedHashMap<>();
            section.put("Y", (byte) sy);
            section.put("block_states", states);
            sections.add(section);
        }
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                int top = surfaceY.applyAsInt(x, z) + (glassAt00 && x == 0 && z == 0 ? 1 : 0);
                heights[z * 16 + x] = top + 1 - minY;
            }
        }
        long[] packed = new long[37]; // 9 bits, 7 per long
        for (int i = 0; i < 256; i++) {
            packed[i / 7] |= (long) heights[i] << ((i % 7) * 9);
        }
        Map<String, Object> chunk = new LinkedHashMap<>();
        chunk.put("xPos", chunkX);
        chunk.put("zPos", chunkZ);
        chunk.put("yPos", -4);
        chunk.put("Status", "minecraft:full");
        chunk.put("sections", sections);
        chunk.put("Heightmaps", Map.of("WORLD_SURFACE", packed));
        return chunk;
    }

    /** Writes a region file holding {@code chunks}, keyed by local index x + z * 32. */
    public static void writeRegion(Path file, Map<Integer, Map<String, Object>> chunks) throws IOException {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        byte[] header = new byte[8192];
        int sector = 2;
        for (Map.Entry<Integer, Map<String, Object>> e : chunks.entrySet()) {
            ByteArrayOutputStream compressed = new ByteArrayOutputStream();
            try (DeflaterOutputStream z = new DeflaterOutputStream(compressed)) {
                z.write(nbt(e.getValue()));
            }
            byte[] c = compressed.toByteArray();
            int length = c.length + 1;
            int sectors = (length + 4 + 4095) / 4096;
            byte[] slot = new byte[sectors * 4096];
            slot[0] = (byte) (length >>> 24);
            slot[1] = (byte) (length >>> 16);
            slot[2] = (byte) (length >>> 8);
            slot[3] = (byte) length;
            slot[4] = 2;
            System.arraycopy(c, 0, slot, 5, c.length);
            body.write(slot);
            int h = e.getKey() * 4;
            header[h] = (byte) (sector >>> 16);
            header[h + 1] = (byte) (sector >>> 8);
            header[h + 2] = (byte) sector;
            header[h + 3] = (byte) sectors;
            sector += sectors;
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(header);
        out.write(body.toByteArray());
        Files.createDirectories(file.getParent());
        Files.write(file, out.toByteArray());
    }

    static byte[] nbt(Map<String, Object> root) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(10);
        out.writeUTF("");
        writeCompound(out, root);
        return bytes.toByteArray();
    }

    @SuppressWarnings("unchecked")
    private static void writeCompound(DataOutputStream out, Map<String, Object> map) throws IOException {
        for (Map.Entry<String, Object> e : map.entrySet()) {
            out.writeByte(tag(e.getValue()));
            out.writeUTF(e.getKey());
            writePayload(out, e.getValue());
        }
        out.writeByte(0);
    }

    private static int tag(Object v) {
        if (v instanceof Byte) return 1;
        if (v instanceof Integer) return 3;
        if (v instanceof Long) return 4;
        if (v instanceof String) return 8;
        if (v instanceof List) return 9;
        if (v instanceof Map) return 10;
        if (v instanceof long[]) return 12;
        throw new IllegalArgumentException(String.valueOf(v));
    }

    @SuppressWarnings("unchecked")
    private static void writePayload(DataOutputStream out, Object v) throws IOException {
        if (v instanceof Byte b) out.writeByte(b);
        else if (v instanceof Integer i) out.writeInt(i);
        else if (v instanceof Long l) out.writeLong(l);
        else if (v instanceof String s) out.writeUTF(s);
        else if (v instanceof long[] longs) {
            out.writeInt(longs.length);
            for (long l : longs) out.writeLong(l);
        } else if (v instanceof Map) writeCompound(out, (Map<String, Object>) v);
        else if (v instanceof List<?> list) {
            out.writeByte(list.isEmpty() ? 0 : tag(list.get(0)));
            out.writeInt(list.size());
            for (Object o : list) writePayload(out, o);
        }
    }
}
