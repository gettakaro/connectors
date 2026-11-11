package io.takaro.minecraft.core.icons;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.map.PngWriter;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.function.Consumer;

/**
 * Which picture each modded item has and the bytes behind it, for {@code items.icons}. The hashes
 * are computed once per process on its own thread, and the bytes are read back per request, so
 * neither ever touches the game thread or holds every image in memory.
 */
public final class ItemIconCatalog {

    public static final int MAX_BATCH_HASHES = 100;
    public static final int MAX_RESPONSE_BYTES = 1 << 20;
    /** Mod jars share the server's heap: limits on what is read or decoded, above any sensible item texture. */
    private static final long MAX_SOURCE_BYTES = 8L << 20;
    private static final long MAX_SOURCE_PIXELS = 1L << 24;
    private static final int PNG_HEADER_LENGTH = 33;
    private static final byte[] PNG_SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    public record Icon(String hash, byte[] png) {}

    private final ItemTextureResolver resolver;
    private final Consumer<String> info;
    private final Consumer<String> warning;
    private final CompletableFuture<Void> ready = new CompletableFuture<>();
    private volatile Map<String, String> hashByItem = Map.of();
    private volatile Map<String, Path> textureByHash = Map.of();

    /** @param roots the mods' root folders, in an order that does not change between restarts */
    public ItemIconCatalog(List<Path> roots, Consumer<String> info, Consumer<String> warning) {
        this.resolver = new ItemTextureResolver(new ModAssets(roots));
        this.info = info;
        this.warning = warning;
    }

    /** Completes when the hashes are known, also after a failed build. */
    public CompletableFuture<Void> ready() {
        return ready;
    }

    public void buildInBackground(Collection<String> itemCodes) {
        Thread thread = new Thread(() -> build(itemCodes), "takaro-item-icons");
        thread.setDaemon(true);
        thread.start();
    }

    /** The item list could not be read, so no build will ever run; a waiting Item sync goes on without icons. */
    public void abandon(Throwable cause) {
        warning.accept("Item icons are unavailable: " + cause);
        ready.complete(null);
    }

    public void build(Collection<String> itemCodes) {
        try {
            Map<String, String> hashes = new HashMap<>();
            Map<String, Path> textures = new HashMap<>();
            Map<Path, String> hashOfTexture = new HashMap<>();
            for (String code : itemCodes) {
                Path texture = resolver.resolve(code);
                if (texture == null) {
                    continue;
                }
                String hash = hashOfTexture.computeIfAbsent(texture, this::hashOf);
                if (!hash.isEmpty()) {
                    hashes.put(code, hash);
                    textures.put(hash, texture);
                }
            }
            hashByItem = hashes;
            textureByHash = textures;
            info.accept("Item icons: " + hashes.size() + " of " + itemCodes.size() + " items have art ("
                    + textures.size() + " distinct images)");
        } catch (RuntimeException | LinkageError e) {
            warning.accept("Item icons could not be read: " + e);
        } finally {
            ready.complete(null);
        }
    }

    /** The hash to report for this item, or null when it has no art. */
    public String hashFor(String itemCode) {
        return hashByItem.get(itemCode);
    }

    /** Empty means this texture cannot be served, which the caller keeps from Takaro. */
    private String hashOf(Path texture) {
        try {
            byte[] png = readIcon(texture);
            return png != null && png.length <= MAX_RESPONSE_BYTES ? sha256(png) : "";
        } catch (IOException | RuntimeException | LinkageError e) {
            warning.accept("Skipping the item icon " + texture + ": " + e);
            return "";
        }
    }

    /**
     * Answers one request in order: a hash that cannot be served is skipped, and the walk stops
     * at the first icon that would push the decoded total past the bound, so Takaro asks again.
     */
    public List<Icon> serve(List<String> hashes) {
        List<Icon> answer = new ArrayList<>();
        long total = 0;
        for (String hash : hashes) {
            byte[] png = bytesFor(hash);
            if (png == null) {
                continue;
            }
            if (total + png.length > MAX_RESPONSE_BYTES) {
                break;
            }
            total += png.length;
            answer.add(new Icon(hash, png));
        }
        return answer;
    }

    /** Re-checked against the hash: Takaro permanently rejects a mismatch, so a swapped jar must be a skip. */
    private byte[] bytesFor(String hash) {
        Path texture = textureByHash.get(hash);
        if (texture == null) {
            return null;
        }
        try {
            byte[] png = readIcon(texture);
            return png != null && png.length <= MAX_RESPONSE_BYTES && sha256(png).equals(hash) ? png : null;
        } catch (IOException | RuntimeException | LinkageError e) {
            warning.accept("Could not read the item icon " + texture + ": " + e);
            return null;
        }
    }

    private static byte[] readIcon(Path texture) throws IOException {
        if (Files.size(texture) > MAX_SOURCE_BYTES) {
            return null;
        }
        byte[] file = Files.readAllBytes(texture);
        if (!isPng(file)) {
            return null;
        }
        JsonObject animation = animationOf(texture);
        if (animation == null) {
            return file;
        }
        return declaredPixels(file) <= MAX_SOURCE_PIXELS ? firstFrame(file, animation) : null;
    }

    private static boolean isPng(byte[] file) {
        return file.length >= PNG_HEADER_LENGTH
                && Arrays.equals(file, 0, PNG_SIGNATURE.length, PNG_SIGNATURE, 0, PNG_SIGNATURE.length)
                && file[12] == 'I' && file[13] == 'H' && file[14] == 'D' && file[15] == 'R';
    }

    /** Read from the header, so a small file claiming an enormous image is refused before it is decoded. */
    private static long declaredPixels(byte[] file) {
        ByteBuffer header = ByteBuffer.wrap(file);
        return Integer.toUnsignedLong(header.getInt(16)) * Integer.toUnsignedLong(header.getInt(20));
    }

    private static JsonObject animationOf(Path texture) throws IOException {
        Path meta = texture.resolveSibling(texture.getFileName() + ".mcmeta");
        if (!Files.isRegularFile(meta)) {
            return null;
        }
        try {
            JsonElement parsed = JsonParser.parseString(Files.readString(meta, StandardCharsets.UTF_8));
            JsonElement animation = parsed.isJsonObject() ? parsed.getAsJsonObject().get("animation") : null;
            return animation != null && animation.isJsonObject() ? animation.getAsJsonObject() : null;
        } catch (com.google.gson.JsonParseException e) {
            return null;
        }
    }

    /** An animated texture is a strip of frames; a shop picture is its first frame. */
    private static byte[] firstFrame(byte[] file, JsonObject animation) throws IOException {
        BufferedImage strip = ImageIO.read(new ByteArrayInputStream(file));
        if (strip == null) {
            return null;
        }
        int width = strip.getWidth();
        int height = strip.getHeight();
        int side = Math.min(width, height);
        int frameWidth = frameSide(animation, "width", animation.has("height") ? width : side, width);
        int frameHeight = frameSide(animation, "height", animation.has("width") ? height : side, height);
        // PngWriter rather than ImageIO: its output does not depend on the JDK, so the hash holds across upgrades.
        return PngWriter.write(strip.getRGB(0, 0, frameWidth, frameHeight, null, 0, frameWidth), frameWidth, frameHeight);
    }

    private static int frameSide(JsonObject animation, String key, int fallback, int limit) {
        JsonElement value = animation.get(key);
        boolean number = value != null && value.isJsonPrimitive() && value.getAsJsonPrimitive().isNumber();
        int side = number ? value.getAsInt() : fallback;
        return side >= 1 && side <= limit ? side : fallback;
    }

    private static String sha256(byte[] bytes) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }
}
