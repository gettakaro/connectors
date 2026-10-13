package io.takaro.minecraft.core.map;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

/**
 * Reads chunks out of an Anvil region file ({@code r.<x>.<z>.mca}, 32x32 chunks). The file is
 * read whole and read-only, so the server writing it at the same time only costs us a chunk
 * that fails to decode; the next render picks it up.
 */
public final class RegionFile {

    public static final int CHUNKS = 32;
    public static final int BLOCKS = CHUNKS * 16;
    private static final int SECTOR = 4096;
    private static final Pattern NAME = Pattern.compile("^r\\.(-?\\d+)\\.(-?\\d+)\\.mca$");

    private final byte[] bytes;

    private RegionFile(byte[] bytes) {
        this.bytes = bytes;
    }

    public static RegionFile read(Path path) throws IOException {
        return new RegionFile(Files.readAllBytes(path));
    }

    static RegionFile of(byte[] bytes) {
        return new RegionFile(bytes);
    }

    /** The region coordinates in a region file name, or null for any other file. */
    public static int[] coordinates(String fileName) {
        Matcher m = NAME.matcher(fileName);
        if (!m.matches()) return null;
        try {
            return new int[] {Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2))};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /**
     * The chunk at local chunk coordinates (0..31), or null when it was never generated, is
     * stored outside the region file, uses a compression we do not read, or does not decode.
     */
    public Map<String, Object> chunk(int localX, int localZ) {
        int index = 4 * ((localX & 31) + (localZ & 31) * CHUNKS);
        if (bytes.length < 2 * SECTOR) return null;
        int offset = ((bytes[index] & 0xff) << 16) | ((bytes[index + 1] & 0xff) << 8) | (bytes[index + 2] & 0xff);
        int sectors = bytes[index + 3] & 0xff;
        if (offset < 2 || sectors == 0) return null;
        long start = (long) offset * SECTOR;
        if (start + 5 > bytes.length) return null;
        int pos = (int) start;
        int length = ((bytes[pos] & 0xff) << 24) | ((bytes[pos + 1] & 0xff) << 16)
                | ((bytes[pos + 2] & 0xff) << 8) | (bytes[pos + 3] & 0xff);
        int compression = bytes[pos + 4] & 0xff;
        if (length <= 1 || pos + 4L + length > bytes.length) return null;
        if ((compression & 0x80) != 0) return null; // oversized chunk kept in a .mcc file
        InputStream raw = new ByteArrayInputStream(bytes, pos + 5, length - 1);
        try (InputStream in = switch (compression) {
            case 1 -> new GZIPInputStream(raw);
            case 2 -> new InflaterInputStream(raw);
            case 3 -> raw;
            default -> null; // LZ4 or a custom codec
        }) {
            return in == null ? null : Nbt.readRoot(in);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }
}
