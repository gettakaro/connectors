package dev.takaro.hytale.protocol;

import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Properties;
import java.util.regex.Pattern;

/**
 * What this connector tells Takaro about itself and the game: the {@code game} and
 * {@code connector} identify fields and the {@code game.profile} capability with its icon.
 *
 * <p>The connector version (version.properties, stamped by the build) and the icon (the
 * shipped icon-256.png) are both read back from the jar, so neither is ever written by hand
 * and a changed icon always gets a new revision.
 */
public final class GameProfile {
    public static final String GAME = "hytale";
    /** Takaro groups connector releases by this name, so it never changes. */
    public static final String CONNECTOR_NAME = "takaro-hytale";
    public static final String DISPLAY_NAME = "Hytale";

    static final String VERSION_RESOURCE = "/dev/takaro/hytale/version.properties";
    static final String ICON_RESOURCE = "/dev/takaro/hytale/icon-256.png";
    static final int MAX_ICON_BYTES = 128 * 1024;
    static final int MAX_GAME_VERSION_LENGTH = 32;

    // Semver 2.0.0, the only form Takaro accepts for connector.version.
    private static final Pattern SEMVER = Pattern.compile(
        "^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)"
            + "(?:-((?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\\.(?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
            + "(?:\\+([0-9a-zA-Z-]+(?:\\.[0-9a-zA-Z-]+)*))?$");

    private static final String STAMPED_VERSION = loadStampedVersion();
    private static final Icon ICON = loadIcon();

    private GameProfile() {
    }

    /** The shipped icon bytes with the revision and size declared for them. */
    public record Icon(byte[] png, String revision, int size) {
    }

    /** The version the build stamped into version.properties, or null for an unstamped build. */
    public static String stampedVersion() {
        return STAMPED_VERSION;
    }

    /** The icon this jar ships, or null when it cannot be read or is too large for Takaro. */
    public static Icon icon() {
        return ICON;
    }

    public static boolean isSemver(String version) {
        return version != null && version.length() <= 64 && SEMVER.matcher(version).matches();
    }

    /** The running game build as Takaro takes it (1 to 32 printable ASCII characters), or null. */
    public static String gameVersion(String raw) {
        if (raw == null) {
            return null;
        }
        String trimmed = raw.trim();
        if (trimmed.isEmpty() || trimmed.length() > MAX_GAME_VERSION_LENGTH) {
            return null;
        }
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (c < 0x20 || c > 0x7E) {
                return null;
            }
        }
        return trimmed;
    }

    /**
     * The {@code connector} identify field, or null when the version is not strict semver:
     * Takaro fails the whole identify on an invalid one, so it is better left out.
     */
    public static Map<String, Object> connector(String version) {
        if (!isSemver(version)) {
            return null;
        }
        Map<String, Object> connector = new LinkedHashMap<>();
        connector.put("name", CONNECTOR_NAME);
        connector.put("version", version);
        return connector;
    }

    /** The {@code game.profile} capability parameters. */
    public static Map<String, Object> parameters(String rawGameVersion) {
        Map<String, Object> parameters = new LinkedHashMap<>();
        parameters.put("name", DISPLAY_NAME);
        String version = gameVersion(rawGameVersion);
        if (version != null) {
            parameters.put("version", version);
        }
        if (ICON != null) {
            Map<String, Object> icon = new LinkedHashMap<>();
            icon.put("revision", ICON.revision());
            icon.put("size", ICON.size());
            parameters.put("icon", icon);
        }
        return parameters;
    }

    /** The first 12 hex characters of the sha256. */
    public static String revision(byte[] png) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(png);
            return HexFormat.of().formatHex(digest).substring(0, 12);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static Icon loadIcon() {
        byte[] png = read(ICON_RESOURCE);
        if (png == null || png.length == 0 || png.length > MAX_ICON_BYTES) {
            return null;
        }
        return new Icon(png, revision(png), png.length);
    }

    private static String loadStampedVersion() {
        byte[] bytes = read(VERSION_RESOURCE);
        if (bytes == null) {
            return null;
        }
        try {
            Properties p = new Properties();
            p.load(new java.io.ByteArrayInputStream(bytes));
            String v = p.getProperty("version", "").trim();
            return v.isEmpty() || v.startsWith("${") ? null : v;
        } catch (IOException e) {
            return null;
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
