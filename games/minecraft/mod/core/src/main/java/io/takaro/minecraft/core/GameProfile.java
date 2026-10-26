package io.takaro.minecraft.core;

import com.google.gson.JsonObject;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.regex.Pattern;

/**
 * What this connector tells Takaro about itself and the game: the {@code game} and
 * {@code connector} identify fields and the {@code game.profile} capability with its icon.
 *
 * The connector version and the icon are build outputs of core (see core/build.gradle.kts),
 * read back from the classpath, so neither is ever written by hand.
 */
public final class GameProfile {

    public static final String GAME = "minecraft";
    /** One name for every loader: Takaro groups connector releases by it, so it never changes. */
    public static final String CONNECTOR_NAME = "takaro-minecraft";
    public static final String DISPLAY_NAME = "Minecraft";

    static final String VERSION_RESOURCE = "/io/takaro/minecraft/core/connector-version.txt";
    static final String ICON_RESOURCE = "/io/takaro/minecraft/core/icon-256.png";
    static final int MAX_ICON_BYTES = 128 * 1024;
    static final int MAX_GAME_VERSION_LENGTH = 32;

    // Semver 2.0.0, the form Takaro accepts for connector.version.
    private static final Pattern SEMVER = Pattern.compile(
            "^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)"
                    + "(?:-((?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\\.(?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
                    + "(?:\\+([0-9a-zA-Z-]+(?:\\.[0-9a-zA-Z-]+)*))?$");

    private static final String CONNECTOR_VERSION = loadConnectorVersion();
    private static final Icon ICON = loadIcon();

    private GameProfile() {
    }

    /** The shipped icon bytes with the revision and size declared for them. */
    public record Icon(byte[] png, String revision, int size) {
    }

    /** The connector's release version, or null when the jar carries none Takaro would accept. */
    public static String connectorVersion() {
        return CONNECTOR_VERSION;
    }

    /** The icon this jar ships, or null when it cannot be read. */
    public static Icon icon() {
        return ICON;
    }

    static boolean isSemver(String version) {
        return version != null && version.length() <= 64 && SEMVER.matcher(version).matches();
    }

    /** The running game build as Takaro takes it (1 to 32 printable ASCII characters), or null. */
    static String gameVersion(String raw) {
        if (raw == null) return null;
        String trimmed = raw.trim();
        if (trimmed.isEmpty() || trimmed.length() > MAX_GAME_VERSION_LENGTH) return null;
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (c < 0x20 || c > 0x7E) return null;
        }
        return trimmed;
    }

    /** The {@code connector} identify field, or null when the version is unusable. */
    static JsonObject connectorJson() {
        if (CONNECTOR_VERSION == null) return null;
        JsonObject connector = new JsonObject();
        connector.addProperty("name", CONNECTOR_NAME);
        connector.addProperty("version", CONNECTOR_VERSION);
        return connector;
    }

    /** The {@code game.profile} capability parameters. */
    static JsonObject parameters(String rawGameVersion) {
        JsonObject parameters = new JsonObject();
        parameters.addProperty("name", DISPLAY_NAME);
        String version = gameVersion(rawGameVersion);
        if (version != null) {
            parameters.addProperty("version", version);
        }
        if (ICON != null) {
            JsonObject icon = new JsonObject();
            icon.addProperty("revision", ICON.revision());
            icon.addProperty("size", ICON.size());
            parameters.add("icon", icon);
        }
        return parameters;
    }

    private static String loadConnectorVersion() {
        byte[] bytes = read(VERSION_RESOURCE);
        if (bytes == null) return null;
        String version = new String(bytes, StandardCharsets.UTF_8).trim();
        return isSemver(version) ? version : null;
    }

    static Icon loadIcon() {
        byte[] png = read(ICON_RESOURCE);
        if (png == null || png.length == 0 || png.length > MAX_ICON_BYTES) return null;
        return new Icon(png, revision(png), png.length);
    }

    /** The first 12 hex characters of the sha256: a changed icon always gets a new revision. */
    static String revision(byte[] png) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(png);
            return HexFormat.of().formatHex(digest).substring(0, 12);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static byte[] read(String resource) {
        try (InputStream stream = GameProfile.class.getResourceAsStream(resource)) {
            return stream == null ? null : stream.readAllBytes();
        } catch (IOException e) {
            return null;
        }
    }
}
