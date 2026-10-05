package io.takaro.zomboid.core.model;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

public record PlayerInfo(
        String gameId,
        String name,
        String steamId,
        String epicOnlineServicesId,
        String xboxLiveId,
        String platformId,
        String ip,
        int ping
) {
    public static final String PLATFORM_PREFIX = "steam:";
    public static final String LOCAL_ACCOUNT_PREFIX = "zomboid:";

    public static String buildPlatformId(String steamId) {
        return PLATFORM_PREFIX + steamId;
    }

    /**
     * The identity Takaro matches this player on.
     *
     * <p>gameId is always the server username. On a Steam server the player carries the SteamID64
     * as {@code steamId} and {@code steam:<SteamID64>} as {@code platformId}, which is what lets
     * Takaro match the same Steam account from every other game. A {@code -nosteam} server has no
     * Steam id (the engine reports 0); its accounts are username + password rows in that server's
     * own database, so the player gets a server-scoped {@link #localAccountPlatformId}. Without
     * one Takaro rejects the player outright ("At least one platform identifier ... must be
     * provided"), and an unscoped {@code zomboid:<username>} would merge every "admin" of every
     * non-Steam server in the domain into one Takaro player, roles included.
     */
    public static PlayerInfo identify(String username, String name, long steamId64, String localScope,
                                      String ip, int ping) {
        if (steamId64 != 0L) {
            String steamId = Long.toUnsignedString(steamId64);
            return new PlayerInfo(username, name, steamId, null, null, buildPlatformId(steamId), ip, ping);
        }
        return new PlayerInfo(username, name, null, null, null,
                localAccountPlatformId(localScope, username), ip, ping);
    }

    /**
     * {@code zomboid:<username>-<8 hex>} for a non-Steam account: readable, valid for Takaro's
     * {@code ^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$} rule (other characters become {@code _}), and
     * scoped to one server by hashing the server's identity token with the exact username.
     */
    public static String localAccountPlatformId(String localScope, String username) {
        if (username == null || username.isEmpty()) {
            return null;
        }
        String readable = username.replaceAll("[^A-Za-z0-9_-]", "_");
        if (readable.length() > 32) {
            readable = readable.substring(0, 32);
        }
        String scope = localScope != null ? localScope : "";
        return LOCAL_ACCOUNT_PREFIX + readable + "-" + sha256Hex(scope + "\n" + username).substring(0, 8);
    }

    private static String sha256Hex(String value) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder sb = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                sb.append(String.format("%02x", b & 0xff));
            }
            return sb.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }
}
