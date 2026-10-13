package io.takaro.minecraft.core.map;

import java.io.DataInput;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A minimal reader for Minecraft's binary NBT, enough to read chunks from region files.
 * Compounds become {@code Map<String, Object>}, lists {@code List<Object>}, and the array tags
 * {@code byte[]}, {@code int[]} and {@code long[]}. Strings use Java's modified UTF-8, which is
 * what NBT writes.
 */
public final class Nbt {

    private static final int MAX_DEPTH = 512;

    private Nbt() {}

    /** Reads one named root tag and returns its compound value. */
    @SuppressWarnings("unchecked")
    public static Map<String, Object> readRoot(InputStream in) throws IOException {
        DataInput data = new DataInputStream(in);
        int type = data.readUnsignedByte();
        if (type != 10) {
            throw new IOException("NBT root is not a compound (tag " + type + ")");
        }
        data.readUTF();
        return (Map<String, Object>) readPayload(data, type, 0);
    }

    private static Object readPayload(DataInput in, int type, int depth) throws IOException {
        if (depth > MAX_DEPTH) {
            throw new IOException("NBT nested too deeply");
        }
        switch (type) {
            case 1: return in.readByte();
            case 2: return in.readShort();
            case 3: return in.readInt();
            case 4: return in.readLong();
            case 5: return in.readFloat();
            case 6: return in.readDouble();
            case 7: {
                byte[] bytes = new byte[length(in.readInt())];
                in.readFully(bytes);
                return bytes;
            }
            case 8: return in.readUTF();
            case 9: {
                int elementType = in.readUnsignedByte();
                int n = length(in.readInt());
                List<Object> list = new ArrayList<>(Math.min(n, 1024));
                for (int i = 0; i < n; i++) {
                    list.add(readPayload(in, elementType, depth + 1));
                }
                return list;
            }
            case 10: {
                Map<String, Object> map = new HashMap<>();
                while (true) {
                    int childType = in.readUnsignedByte();
                    if (childType == 0) {
                        return map;
                    }
                    String name = in.readUTF();
                    map.put(name, readPayload(in, childType, depth + 1));
                }
            }
            case 11: {
                int[] ints = new int[length(in.readInt())];
                for (int i = 0; i < ints.length; i++) ints[i] = in.readInt();
                return ints;
            }
            case 12: {
                long[] longs = new long[length(in.readInt())];
                for (int i = 0; i < longs.length; i++) longs[i] = in.readLong();
                return longs;
            }
            default:
                throw new IOException("Unknown NBT tag " + type);
        }
    }

    private static int length(int n) throws IOException {
        // A chunk never holds an array anywhere near this; a bigger length means corrupt data.
        if (n < 0 || n > 16 * 1024 * 1024) {
            throw new IOException("Bad NBT length " + n);
        }
        return n;
    }

    // --- typed accessors that tolerate missing or differently typed tags ---

    @SuppressWarnings("unchecked")
    public static Map<String, Object> compound(Map<String, Object> map, String key) {
        Object value = map == null ? null : map.get(key);
        return value instanceof Map ? (Map<String, Object>) value : null;
    }

    @SuppressWarnings("unchecked")
    public static List<Object> list(Map<String, Object> map, String key) {
        Object value = map == null ? null : map.get(key);
        return value instanceof List ? (List<Object>) value : List.of();
    }

    public static long[] longs(Map<String, Object> map, String key) {
        Object value = map == null ? null : map.get(key);
        return value instanceof long[] ? (long[]) value : null;
    }

    public static String string(Map<String, Object> map, String key) {
        Object value = map == null ? null : map.get(key);
        return value instanceof String ? (String) value : null;
    }

    public static Integer integer(Map<String, Object> map, String key) {
        Object value = map == null ? null : map.get(key);
        return value instanceof Number ? ((Number) value).intValue() : null;
    }
}
