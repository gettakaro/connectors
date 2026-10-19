package io.takaro.zomboid.core;

import com.google.gson.JsonObject;

import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Properties;

/**
 * What the connector tells Takaro about itself and the game: the {@code game} and
 * {@code connector} identify fields, and the {@code game.profile} capability with the icon
 * served by {@code getGameIcon}. The connector version and the icon bytes are build outputs
 * (the {@code takaro-connector.properties} resource written from {@code version.txt}, and
 * {@code games/zomboid/assets/icon-256.png}); the icon revision is derived from its bytes.
 */
public final class GameProfile {

    public static final String GAME = "zomboid";
    public static final String CONNECTOR_NAME = "takaro-zomboid";
    public static final String DISPLAY_NAME = "Project Zomboid";

    static final String VERSION_RESOURCE = "/io/takaro/zomboid/core/takaro-connector.properties";
    static final String ICON_RESOURCE = "/io/takaro/zomboid/core/icon-256.png";
    static final int MAX_GAME_VERSION = 32;

    private static final String CONNECTOR_VERSION = loadConnectorVersion();
    private static final byte[] ICON = loadIcon();
    private static final String ICON_REVISION = ICON != null ? revisionOf(ICON) : null;

    private GameProfile() {
    }

    /** The release version this jar was built as, or {@code null} for a build without it. */
    public static String connectorVersion() {
        return CONNECTOR_VERSION;
    }

    /** The shipped icon, or {@code null} when the resource is missing. */
    static byte[] icon() {
        return ICON != null ? ICON.clone() : null;
    }

    static String iconRevision() {
        return ICON_REVISION;
    }

    /** The identify {@code connector} object; {@code null} when the version is unknown. */
    static JsonObject connectorJson() {
        if (CONNECTOR_VERSION == null) {
            return null;
        }
        JsonObject connector = new JsonObject();
        connector.addProperty("name", CONNECTOR_NAME);
        connector.addProperty("version", CONNECTOR_VERSION);
        return connector;
    }

    /** The {@code game.profile} capability parameters for the given running game build. */
    static JsonObject parameters(String gameVersion) {
        JsonObject parameters = new JsonObject();
        parameters.addProperty("name", DISPLAY_NAME);
        String version = sanitizeGameVersion(gameVersion);
        if (version != null) {
            parameters.addProperty("version", version);
        }
        if (ICON != null) {
            JsonObject icon = new JsonObject();
            icon.addProperty("revision", ICON_REVISION);
            icon.addProperty("size", ICON.length);
            parameters.add("icon", icon);
        }
        return parameters;
    }

    /**
     * Takaro takes 1 to 32 printable ASCII characters. Anything else is dropped rather than
     * sent, because an invalid {@code game.profile} rejects the whole manifest.
     */
    static String sanitizeGameVersion(String raw) {
        if (raw == null) {
            return null;
        }
        StringBuilder out = new StringBuilder();
        for (char c : raw.trim().toCharArray()) {
            if (c >= 0x20 && c <= 0x7E) {
                out.append(c);
            }
        }
        String version = out.toString().trim();
        if (version.length() > MAX_GAME_VERSION) {
            version = version.substring(0, MAX_GAME_VERSION).trim();
        }
        return version.isEmpty() ? null : version;
    }

    static String revisionOf(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            return HexFormat.of().formatHex(digest).substring(0, 12);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static String loadConnectorVersion() {
        try (InputStream in = GameProfile.class.getResourceAsStream(VERSION_RESOURCE)) {
            if (in == null) {
                return null;
            }
            Properties properties = new Properties();
            properties.load(in);
            String version = properties.getProperty("version", "").trim();
            return version.isEmpty() ? null : version;
        } catch (IOException e) {
            return null;
        }
    }

    private static byte[] loadIcon() {
        try (InputStream in = GameProfile.class.getResourceAsStream(ICON_RESOURCE)) {
            return in != null ? in.readAllBytes() : null;
        } catch (IOException e) {
            return null;
        }
    }
}
