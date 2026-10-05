package io.takaro.minecraft.core.model;

import org.junit.jupiter.api.Test;

import java.util.UUID;
import java.util.regex.Pattern;

import static org.junit.jupiter.api.Assertions.*;

/** The identity fields Takaro matches players on (steamId, epicOnlineServicesId, xboxLiveId, platformId). */
class PlayerInfoIdentityTest {

    // Takaro's IGamePlayer.platformId validator.
    private static final Pattern TAKARO_PLATFORM_ID = Pattern.compile("^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$");

    @Test
    void javaOnlineAccountGetsMinecraftPlatformIdAndNoXuid() {
        String uuid = "069a79f4-44e9-4726-a5be-fca90e38aaf5";
        PlayerInfo p = PlayerInfo.of(uuid, "Notch", "1.2.3.4", 10);
        assertEquals(uuid, p.gameId());
        assertEquals("minecraft:" + uuid, p.platformId());
        assertTrue(TAKARO_PLATFORM_ID.matcher(p.platformId()).matches());
        assertNull(p.steamId());
        assertNull(p.epicOnlineServicesId());
        assertNull(p.xboxLiveId());
    }

    @Test
    void offlineModeUuidIsNotMistakenForBedrock() {
        String uuid = "562a20ac-581b-3612-8ae9-702103b6379b";
        assertNull(PlayerInfo.xboxLiveIdFromUuid(uuid));
        assertEquals("minecraft:" + uuid, PlayerInfo.of(uuid, "Actor", "", 0).platformId());
    }

    @Test
    void floodgateBedrockPlayerCarriesXuidAsXboxLiveId() {
        long xuid = 2535428651234567L;
        String uuid = new UUID(0L, xuid).toString();
        assertEquals("00000000-0000-0000-0009-01f57c1c3507", uuid);
        PlayerInfo p = PlayerInfo.of(uuid, ".BedrockSteve", "", 0);
        assertEquals("2535428651234567", p.xboxLiveId());
        assertEquals(uuid, p.gameId());
        assertEquals("minecraft:" + uuid, p.platformId());
        assertTrue(TAKARO_PLATFORM_ID.matcher(p.platformId()).matches());
        assertNull(p.steamId());
    }

    @Test
    void malformedOrZeroUuidYieldsNoXuid() {
        assertNull(PlayerInfo.xboxLiveIdFromUuid(null));
        assertNull(PlayerInfo.xboxLiveIdFromUuid("not-a-uuid"));
        assertNull(PlayerInfo.xboxLiveIdFromUuid(new UUID(0L, 0L).toString()));
    }
}
