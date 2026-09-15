package io.takaro.minecraft.core.model;

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
}
