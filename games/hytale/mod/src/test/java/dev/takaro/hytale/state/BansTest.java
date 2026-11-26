package dev.takaro.hytale.state;

import org.junit.jupiter.api.Test;

import java.time.Instant;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class BansTest {

    @Test
    void expiresAtParsesIsoInstants() {
        assertEquals(Instant.parse("2026-12-31T23:59:59Z"), Bans.parseExpiresAt("2026-12-31T23:59:59Z"));
        assertEquals(Instant.parse("2026-09-21T18:00:00Z"), Bans.parseExpiresAt("2026-09-21T20:00:00+02:00"));
    }

    @Test
    void absentOrBlankExpiryMeansPermanent() {
        assertNull(Bans.parseExpiresAt(null));
        assertNull(Bans.parseExpiresAt(""));
        assertNull(Bans.parseExpiresAt("   "));
        assertFalse(Bans.isUnparseableExpiry(null));
        assertFalse(Bans.isUnparseableExpiry(""));
    }

    @Test
    void garbageExpiryIsReportedRatherThanSilentlyIgnored() {
        assertNull(Bans.parseExpiresAt("tomorrow"));
        assertTrue(Bans.isUnparseableExpiry("tomorrow"));
    }

    @Test
    void iBanShapeMatchesWhatTakaroExpects() {
        Map<String, Object> ban = Bans.toIBan(
            "00000000-0000-4000-8000-000000000001", "ExamplePlayer", "griefing",
            Instant.parse("2026-12-31T23:59:59Z"));

        @SuppressWarnings("unchecked")
        Map<String, Object> player = (Map<String, Object>) ban.get("player");
        assertEquals("00000000-0000-4000-8000-000000000001", player.get("gameId"));
        assertEquals("ExamplePlayer", player.get("name"));
        assertEquals("hytale:00000000-0000-4000-8000-000000000001", player.get("platformId"));
        assertEquals("griefing", ban.get("reason"));
        assertEquals("2026-12-31T23:59:59Z", ban.get("expiresAt"));
    }

    @Test
    void permanentBanHasNullExpiresAtAndFallsBackToTheGameIdAsName() {
        Map<String, Object> ban = Bans.toIBan("aaaa0000-0000-0000-0000-000000000001", null, null, null);
        assertTrue(ban.containsKey("expiresAt"));
        assertNull(ban.get("expiresAt"));
        assertNull(ban.get("reason"));
        @SuppressWarnings("unchecked")
        Map<String, Object> player = (Map<String, Object>) ban.get("player");
        // name must never be null - Takaro's BanDTO rejects the whole response.
        assertEquals("aaaa0000-0000-0000-0000-000000000001", player.get("name"));
    }

    @Test
    void banPlayerNameIsNeverNull() {
        // Takaro's BanDTO validates player.name with isString; null fails the whole response.
        @SuppressWarnings("unchecked")
        Map<String, Object> p1 = (Map<String, Object>) Bans.toIBan(
            "d34db33f-0000-4000-8000-000000000001", null, "r", null).get("player");
        assertEquals("d34db33f-0000-4000-8000-000000000001", p1.get("name"));

        @SuppressWarnings("unchecked")
        Map<String, Object> p2 = (Map<String, Object>) Bans.toIBan("abc", "   ", "r", null).get("player");
        assertEquals("abc", p2.get("name"));

        @SuppressWarnings("unchecked")
        Map<String, Object> p3 = (Map<String, Object>) Bans.toIBan("abc", "ExamplePlayer", "r", null).get("player");
        assertEquals("ExamplePlayer", p3.get("name"));
    }

    @Test
    void takaroPermanentSentinelBecomesAPermanentBan() {
        java.time.Instant now = java.time.Instant.parse("2026-11-26T21:30:00Z");
        assertNull(Bans.gameExpiry(java.time.Instant.parse("3021-01-01T00:00:00Z"), now));
        assertNull(Bans.gameExpiry(null, now));
        java.time.Instant soon = java.time.Instant.parse("2026-11-26T21:35:00Z");
        assertEquals(soon, Bans.gameExpiry(soon, now));
        java.time.Instant tenYears = java.time.Instant.parse("2036-11-26T21:30:00Z");
        assertEquals(tenYears, Bans.gameExpiry(tenYears, now));
    }

    @Test
    void banDisconnectTextNamesTheReasonAndExpiry() {
        assertEquals("You are permanently banned! Reason: griefing", Bans.disconnectText("griefing", null));
        assertEquals("You are banned until 2026-11-26 21:35 UTC. Reason: spam",
            Bans.disconnectText("spam", java.time.Instant.parse("2026-11-26T21:35:42Z")));
        assertEquals("You are permanently banned! Reason: No reason.", Bans.disconnectText(" ", null));
    }
}
