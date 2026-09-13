package io.takaro.zomboid.agent;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Path;
import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class BanStoreTest {

    @Test
    void putListRemoveRoundTrips(@TempDir Path dir) {
        Path file = dir.resolve("bans.json");
        BanStore store = new BanStore(file);
        store.put("alice", "7656119", "Alice", "griefing", "2099-01-01T00:00:00Z");

        List<BanStore.Ban> bans = store.list();
        assertEquals(1, bans.size());
        assertEquals("alice", bans.get(0).gameId);
        assertEquals("7656119", bans.get(0).steamId);

        assertNotNull(store.remove("alice"));
        assertTrue(store.list().isEmpty());
    }

    @Test
    void persistsAcrossReload(@TempDir Path dir) {
        Path file = dir.resolve("bans.json");
        new BanStore(file).put("bob", null, "Bob", "cheating", null);

        BanStore reloaded = new BanStore(file);
        assertEquals(1, reloaded.list().size());
        assertEquals("bob", reloaded.get("bob").gameId);
        assertNull(reloaded.get("bob").expiresAt);
    }

    @Test
    void expiredReturnsOnlyPastTimedBans(@TempDir Path dir) {
        BanStore store = new BanStore(dir.resolve("bans.json"));
        Instant now = Instant.now();
        store.put("past", null, "Past", "x", now.minus(1, ChronoUnit.MINUTES).toString());
        store.put("future", null, "Future", "x", now.plus(1, ChronoUnit.HOURS).toString());
        store.put("permanent", null, "Perm", "x", null);

        List<BanStore.Ban> expired = store.expired(now);
        assertEquals(1, expired.size());
        assertEquals("past", expired.get(0).gameId);
    }

    @Test
    void unparseableExpiryIsTreatedAsPermanent(@TempDir Path dir) {
        BanStore store = new BanStore(dir.resolve("bans.json"));
        store.put("weird", null, "Weird", "x", "not-a-date");
        assertTrue(store.expired(Instant.now()).isEmpty());
    }
}
