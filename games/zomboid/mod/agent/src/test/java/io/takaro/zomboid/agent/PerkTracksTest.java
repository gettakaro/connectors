package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.ProgressionTrack;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class PerkTracksTest {

    private static final String KEY_PATTERN = "^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9-]*)*$";

    @Test
    void gamePerkIdsBecomeLowerCamelKeys() {
        assertEquals("axe", PerkTracks.key("Axe"));
        assertEquals("longBlade", PerkTracks.key("LongBlade"));
        assertEquals("plantScavenging", PerkTracks.key("PlantScavenging"));
    }

    @Test
    void modPerkIdsAreCleanedUpToAValidKey() {
        for (String id : new String[] {"My_Perk", "my perk 2", "9Lives", "-x", "Über"}) {
            String key = PerkTracks.key(id);
            assertNotNull(key, id);
            assertTrue(key.matches(KEY_PATTERN), key);
        }
        assertEquals("myPerk", PerkTracks.key("My_Perk"));
    }

    @Test
    void idsWithoutAValidKeyGiveNoTrack() {
        assertNull(PerkTracks.key(null));
        assertNull(PerkTracks.key(""));
        assertNull(PerkTracks.key("___"));
        assertNull(PerkTracks.key("x".repeat(129)));
        assertNull(PerkTracks.track("123", "Numbers", 1, 1f, 0f, 10f));
    }

    @Test
    void aPerkCannotClaimTheReservedCharacterKey() {
        assertNull(PerkTracks.key("Character"));
    }

    @Test
    void progressIsTheFractionOfTheCurrentLevel() {
        ProgressionTrack track = PerkTracks.track("Axe", "Axe", 1, 100f, 75f, 100f);
        assertEquals("axe", track.key());
        assertEquals(1, track.level());
        assertEquals(100.0, track.experience());
        assertEquals(0.25, track.progress());
    }

    @Test
    void progressStaysInRangeWhenExperienceDisagreesWithTheLevel() {
        assertEquals(0.0, PerkTracks.track("Fitness", null, 5, 0f, 1000f, 500f).progress());
        assertEquals(1.0, PerkTracks.track("Fitness", null, 5, 9000f, 1000f, 500f).progress());
    }

    @Test
    void theTopLevelHasNoProgress() {
        ProgressionTrack track = PerkTracks.track("Axe", "Axe", 10, 37500f, 37500f, -1f);
        assertEquals(37500.0, track.experience());
        assertNull(track.progress());
    }

    @Test
    void experienceKeepsTheFloatsShortText() {
        assertEquals(12.3, PerkTracks.track("Axe", null, 0, 12.3f, 0f, 75f).experience());
    }

    @Test
    void aNonFiniteExperienceIsLeftOut() {
        ProgressionTrack track = PerkTracks.track("Axe", null, 1, Float.NaN, 75f, 100f);
        assertNull(track.experience());
        assertNull(track.progress());
    }

    @Test
    void aBlankNameIsLeftOutAndALongOneIsCut() {
        assertNull(PerkTracks.track("Axe", "  ", 0, 0f, 0f, 75f).name());
        assertEquals(128, PerkTracks.track("Axe", "n".repeat(200), 0, 0f, 0f, 75f).name().length());
    }
}
