package io.takaro.zomboid.core.model;

import org.junit.jupiter.api.Test;

import java.util.regex.Pattern;

import static org.junit.jupiter.api.Assertions.*;

/** The identity fields Takaro matches players on (steamId, epicOnlineServicesId, xboxLiveId, platformId). */
class PlayerInfoIdentityTest {

    // Takaro's IGamePlayer.platformId validator.
    private static final Pattern TAKARO_PLATFORM_ID = Pattern.compile("^[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+$");

    @Test
    void steamPlayerCarriesSteamId64AndCanonicalPlatformId() {
        PlayerInfo p = PlayerInfo.identify("takarotester", "Limon", 76561198000000001L, "srv", "1.2.3.4", 3);
        assertEquals("takarotester", p.gameId());
        assertEquals("76561198000000001", p.steamId());
        assertEquals("steam:76561198000000001", p.platformId());
        assertNull(p.epicOnlineServicesId());
        assertNull(p.xboxLiveId());
    }

    @Test
    void noSteamAccountGetsServerScopedPlatformId() {
        PlayerInfo p = PlayerInfo.identify("bob", "Bob", 0L, "takaro-dev-zomboid", null, 0);
        assertEquals("bob", p.gameId());
        assertNull(p.steamId());
        assertNotNull(p.platformId());
        assertTrue(p.platformId().startsWith("zomboid:bob-"), p.platformId());
        assertTrue(TAKARO_PLATFORM_ID.matcher(p.platformId()).matches(), p.platformId());
    }

    @Test
    void steamServerPlayerWithoutSteamIdGetsNoLocalId() {
        PlayerInfo p = PlayerInfo.identify("bob", "Bob", 0L, null, null, 0);
        assertEquals("bob", p.gameId());
        assertNull(p.steamId());
        assertNull(p.platformId());
    }

    @Test
    void localAccountIdIsStablePerServerAndDistinctAcrossServers() {
        String a1 = PlayerInfo.localAccountPlatformId("server-a", "admin");
        String a2 = PlayerInfo.localAccountPlatformId("server-a", "admin");
        String b = PlayerInfo.localAccountPlatformId("server-b", "admin");
        assertEquals(a1, a2);
        assertNotEquals(a1, b);
        assertNotEquals(a1, PlayerInfo.localAccountPlatformId("server-a", "Admin"));
    }

    @Test
    void usernamesWithDisallowedCharactersStillProduceAValidPlatformId() {
        String id = PlayerInfo.localAccountPlatformId(null, "Mr. Smith ÄÖ");
        assertTrue(TAKARO_PLATFORM_ID.matcher(id).matches(), id);
        assertNotEquals(id, PlayerInfo.localAccountPlatformId(null, "Mr__Smith___"));
        assertNull(PlayerInfo.localAccountPlatformId("s", ""));
    }
}
