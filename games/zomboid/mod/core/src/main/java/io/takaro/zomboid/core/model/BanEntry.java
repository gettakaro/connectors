package io.takaro.zomboid.core.model;

/**
 * One row of listBans. {@code steamId} (SteamID64, or null) is sent alongside gameId so Takaro can
 * resolve the banned player by Steam account: a ban row keyed only by gameId resolves only when
 * the player already has a profile on this server.
 */
public record BanEntry(String gameId, String name, String reason, String expiresAt, String steamId) {
    public BanEntry(String gameId, String name, String reason, String expiresAt) {
        this(gameId, name, reason, expiresAt, null);
    }
}
