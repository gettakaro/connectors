package dev.takaro.hytale.state;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class KnownPlayersTest {

    private static final String ID = "00000000-0000-4000-8000-000000000001";

    @Test
    void recordsAndReadsBackAPlayer(@TempDir Path dir) {
        KnownPlayers ledger = new KnownPlayers(dir.resolve("known-players.json"));
        ledger.record(ID, "ExamplePlayer", "hytale:" + ID, "203.0.113.7");

        assertEquals("ExamplePlayer", ledger.nameOf(ID));
        assertEquals(ID, ledger.getByName("exampleplayer").gameId);
        assertEquals(1, ledger.size());
    }

    @Test
    void survivesAServerRestart(@TempDir Path dir) {
        Path file = dir.resolve("known-players.json");
        new KnownPlayers(file).record(ID, "ExamplePlayer", "hytale:" + ID, "203.0.113.7");

        KnownPlayers reloaded = new KnownPlayers(file);
        assertEquals("ExamplePlayer", reloaded.nameOf(ID));
        assertEquals("203.0.113.7", reloaded.get(ID).ip);
    }

    @Test
    void producesTheIGamePlayerShape(@TempDir Path dir) {
        KnownPlayers ledger = new KnownPlayers(dir.resolve("p.json"));
        ledger.record(ID, "ExamplePlayer", null, "10.0.0.1");

        Map<String, Object> player = ledger.toGamePlayer(ID);
        assertEquals(ID, player.get("gameId"));
        assertEquals("ExamplePlayer", player.get("name"));
        assertEquals("hytale:" + ID, player.get("platformId"));
        assertEquals("10.0.0.1", player.get("ip"));
    }

    @Test
    void unknownPlayerIsNullNotAFakeEntry(@TempDir Path dir) {
        KnownPlayers ledger = new KnownPlayers(dir.resolve("p.json"));
        assertNull(ledger.get("nope"));
        assertNull(ledger.nameOf("nope"));
        assertNull(ledger.toGamePlayer("nope"));
        assertNull(ledger.getByName("nobody"));
    }

    @Test
    void aCorruptLedgerDoesNotStopTheConnector(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("known-players.json");
        Files.writeString(file, "{ this is not json");

        KnownPlayers ledger = new KnownPlayers(file);
        assertEquals(0, ledger.size());
        ledger.record(ID, "Someone", null, null);
        assertEquals("Someone", ledger.nameOf(ID));
    }

    @Test
    void refreshingKeepsTheOldNameWhenTheNewOneIsMissing(@TempDir Path dir) {
        KnownPlayers ledger = new KnownPlayers(dir.resolve("p.json"));
        ledger.record(ID, "ExamplePlayer", null, "10.0.0.1");
        ledger.record(ID, null, null, null);
        assertEquals("ExamplePlayer", ledger.nameOf(ID));
        assertEquals("10.0.0.1", ledger.get(ID).ip);
    }

    @Test
    void recordWithoutAGameIdIsIgnored(@TempDir Path dir) {
        KnownPlayers ledger = new KnownPlayers(dir.resolve("p.json"));
        assertNull(ledger.record(null, "x", null, null));
        assertNull(ledger.record("", "x", null, null));
        assertEquals(0, ledger.size());
    }

    @Test
    void synthesizesAMinimalPlayerForANeverSeenId() {
        java.util.Map<String, Object> p = KnownPlayers.synthesize("00000000-0000-0000-0000-000000000000");
        assertNotNull(p);
        assertEquals("00000000-0000-0000-0000-000000000000", p.get("gameId"));
        // name must be a non-empty String or Takaro 400s the whole IGamePlayer
        assertTrue(p.get("name") instanceof String);
        assertFalse(((String) p.get("name")).isEmpty());
        assertEquals("hytale:00000000-0000-0000-0000-000000000000", p.get("platformId"));
        assertEquals(Boolean.FALSE, p.get("online"));
    }

    @Test
    void synthesizeNeedsAnId() {
        assertNull(KnownPlayers.synthesize(null));
        assertNull(KnownPlayers.synthesize(""));
    }
}
