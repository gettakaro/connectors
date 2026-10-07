package io.takaro.minecraft.core.model;

import java.util.UUID;

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
    public static final String PLATFORM_PREFIX = "minecraft:";

    public static String buildPlatformId(String uuid) {
        return PLATFORM_PREFIX + uuid;
    }

    /**
     * The Xbox Live XUID of a Bedrock player joining through Geyser + Floodgate, or null.
     *
     * <p>Floodgate gives every Bedrock player the Java UUID {@code new UUID(0, xuid)}: the most
     * significant 64 bits are zero and the least significant 64 bits are the XUID. No Mojang
     * account UUID (online v4 or offline v3) has zero high bits, so the shape alone identifies a
     * Floodgate player without a compile-time dependency on Floodgate. Sending the XUID as
     * {@code xboxLiveId} lets Takaro match the player with the same Xbox account from other games.
     */
    public static String xboxLiveIdFromUuid(String uuid) {
        if (uuid == null) {
            return null;
        }
        UUID parsed;
        try {
            parsed = UUID.fromString(uuid);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if (parsed.getMostSignificantBits() != 0L || parsed.getLeastSignificantBits() <= 0L) {
            return null;
        }
        return Long.toString(parsed.getLeastSignificantBits());
    }

    /** The identity every loader reports for a player with this UUID. */
    public static PlayerInfo of(String uuid, String name, String ip, int ping) {
        return new PlayerInfo(
                uuid,
                name,
                null,
                null,
                xboxLiveIdFromUuid(uuid),
                buildPlatformId(uuid),
                ip,
                ping
        );
    }
}
