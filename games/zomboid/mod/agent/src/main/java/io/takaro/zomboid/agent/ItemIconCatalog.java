package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.ItemIcon;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/**
 * The PNG art behind each item icon, for {@code items.icons}. Game-class-free. Hashes are
 * taken once per process from the file bytes, so they stay the same across restarts while
 * a file is unchanged; the bytes are read back per request and never kept in memory.
 */
public final class ItemIconCatalog {

    /** Decoded PNG bytes allowed per icon and per {@code getItemIcons} response. */
    static final int MAX_RESPONSE_BYTES = 1 << 20;
    private static final byte[] PNG_SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    private final Consumer<String> log;
    private volatile Map<String, String> hashByIcon = Map.of();
    private volatile Map<String, Path> fileByHash = Map.of();

    public ItemIconCatalog(Consumer<String> log) {
        this.log = log;
    }

    /** @param files icon name to the file holding its art */
    public void build(Map<String, String> files) {
        Map<String, String> hashes = new HashMap<>();
        Map<String, Path> paths = new HashMap<>();
        Map<String, String> hashOfFile = new HashMap<>();
        for (Map.Entry<String, String> entry : files.entrySet()) {
            String hash = hashOfFile.computeIfAbsent(entry.getValue(), this::hashOf);
            if (!hash.isEmpty()) {
                hashes.put(entry.getKey(), hash);
                paths.put(hash, Path.of(entry.getValue()));
            }
        }
        hashByIcon = hashes;
        fileByHash = paths;
        log.accept("Item icons: " + hashes.size() + " of " + files.size() + " icon files can be served ("
                + paths.size() + " distinct images)");
    }

    /** The hash to report for this icon, or null when it has no servable art. */
    public String hashFor(String icon) {
        return icon == null ? null : hashByIcon.get(icon);
    }

    /**
     * Answers one request in order: a hash that cannot be served is skipped, and the walk
     * stops at the first icon that would push the decoded total past the bound, so Takaro
     * asks again for the rest.
     */
    public List<ItemIcon> serve(List<String> hashes) {
        List<ItemIcon> answer = new ArrayList<>();
        long total = 0;
        for (String hash : hashes) {
            byte[] png = readServable(hash);
            if (png == null) {
                continue;
            }
            if (total + png.length > MAX_RESPONSE_BYTES) {
                break;
            }
            total += png.length;
            answer.add(new ItemIcon(hash, png));
        }
        return answer;
    }

    /** Null when the hash is unknown, or its file changed, went away or grew past the bound. */
    private byte[] readServable(String hash) {
        Path file = fileByHash.get(hash);
        if (file == null) {
            return null;
        }
        byte[] png = readIcon(file);
        return png != null && hash.equals(sha256(png)) ? png : null;
    }

    private String hashOf(String file) {
        byte[] png = readIcon(Path.of(file));
        return png == null ? "" : sha256(png);
    }

    private byte[] readIcon(Path file) {
        try {
            if (Files.size(file) > MAX_RESPONSE_BYTES) {
                log.accept("Skipping the item icon " + file + ": larger than 1 MiB");
                return null;
            }
            byte[] bytes = Files.readAllBytes(file);
            if (bytes.length > MAX_RESPONSE_BYTES
                    || !Arrays.equals(bytes, 0, Math.min(bytes.length, PNG_SIGNATURE.length), PNG_SIGNATURE, 0, PNG_SIGNATURE.length)) {
                log.accept("Skipping the item icon " + file + ": not a PNG");
                return null;
            }
            return bytes;
        } catch (IOException e) {
            log.accept("Skipping the item icon " + file + ": " + e);
            return null;
        }
    }

    private static String sha256(byte[] bytes) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }
}
