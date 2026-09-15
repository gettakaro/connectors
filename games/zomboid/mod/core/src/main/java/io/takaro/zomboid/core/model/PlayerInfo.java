package io.takaro.zomboid.core.model;

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

    public static String buildPlatformId(String steamId) {
        return PLATFORM_PREFIX + steamId;
    }
}
