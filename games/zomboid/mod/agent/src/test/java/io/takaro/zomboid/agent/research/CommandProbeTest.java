package io.takaro.zomboid.agent.research;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import org.junit.jupiter.api.Test;

class CommandProbeTest {

    private static final Set<String> NATIVE = Set.of("players", "help", "kick");
    private static final CommandProbe.Caller CALLER =
            new CommandProbe.Caller("tester", 1L, "user", 1, 7L);

    private final List<String> replies = Collections.synchronizedList(new ArrayList<>());
    private final List<JsonObject> receipts = Collections.synchronizedList(new ArrayList<>());
    private final List<JsonObject> sent = Collections.synchronizedList(new ArrayList<>());
    private final ExecutorService direct = new DirectExecutor();

    private CommandProbe probe(CommandProbe.Backend backend) {
        return new CommandProbe(NATIVE::contains, backend, (conn, text) -> replies.add(conn + ":" + text),
                receipts::add, direct);
    }

    private static JsonObject catalog(String json) {
        return JsonParser.parseString(json).getAsJsonObject();
    }

    private CommandProbe.Backend ok() {
        return inv -> {
            sent.add(inv);
            return new CommandProbe.Outcome("ok", List.of("hi " + inv.getAsJsonObject("caller").get("username").getAsString()));
        };
    }

    @Test
    void claimsCatalogCommandAndRepliesOnceToTheCaller() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe',installationId:'i1'}]}"));
        assertTrue(p.claim("/tprobe a \"b c\"", "conn1", CALLER));
        assertEquals(List.of("conn1:hi tester"), replies);
        assertEquals(List.of("a", "b c"),
                List.of(sent.get(0).getAsJsonArray("args").get(0).getAsString(),
                        sent.get(0).getAsJsonArray("args").get(1).getAsString()));
        assertEquals("ok", receipts.get(0).get("outcome").getAsString());
    }

    @Test
    void unknownInputIsLeftToTheGame() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertFalse(p.claim("/nosuch", "c", CALLER));
        assertFalse(p.claim("hello", "c", CALLER));
        assertFalse(p.claim("/", "c", CALLER));
        assertTrue(replies.isEmpty());
    }

    @Test
    void namesMatchCaseInsensitivelyAndCollidingRowsFanOut() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'TProbe',installationId:'i1'},"
                + "{name:'tprobe',installationId:'i2'}]}"));
        assertEquals(2, p.snapshot().byName().get("tprobe").size());
        assertTrue(p.claim("/TPROBE", "c", CALLER));
        assertEquals(2, sent.get(0).getAsJsonArray("installations").size());
        assertEquals(1, replies.size());
    }

    @Test
    void gameOwnedNamesAreRefused() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'Players'},{name:'all'},{name:'roll'},"
                + "{name:'rollcall'},{name:'cards'},{name:'safehouse'},{name:'two words'},{name:'ok_1'}]}"));
        assertEquals(Set.of("ok_1"), p.snapshot().byName().keySet());
        assertEquals(7, p.snapshot().refused().size());
        assertFalse(p.claim("/players", "c", CALLER));
    }

    @Test
    void malformedArgumentsFailPrivatelyWithoutCallingTheBackend() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertTrue(p.claim("/tprobe \"open", "c", CALLER));
        assertTrue(sent.isEmpty());
        assertEquals(List.of("c:" + CommandProbe.MALFORMED), replies);
    }

    @Test
    void offlineBackendKeepsTheCatalogAndFailsPrivately() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        p.applyCatalog(null);
        assertFalse(p.online());
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertTrue(sent.isEmpty());
        assertEquals(List.of("c:" + CommandProbe.UNAVAILABLE), replies);
        assertEquals("offline", receipts.get(0).get("outcome").getAsString());
    }

    @Test
    void nothingIsClaimedBeforeTheFirstCatalog() {
        CommandProbe p = probe(ok());
        p.applyCatalog(null);
        assertFalse(p.claim("/tprobe", "c", CALLER));
    }

    @Test
    void backendFailureIsAnUnknownOutcomeAndIsNotRetried() {
        int[] calls = {0};
        CommandProbe p = probe(inv -> {
            calls[0]++;
            throw new java.net.http.HttpTimeoutException("slow");
        });
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertEquals(1, calls[0]);
        assertEquals(List.of("c:" + CommandProbe.UNKNOWN_OUTCOME), replies);
    }

    @Test
    void deniedIsPassedThroughPrivately() {
        CommandProbe p = probe(inv -> new CommandProbe.Outcome("denied", List.of("You may not run tprobe.")));
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertEquals(List.of("c:You may not run tprobe."), replies);
        assertEquals("denied", receipts.get(0).get("outcome").getAsString());
    }

    @Test
    void addChangeRemoveAndStaleRevisions() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:2,commands:[{name:'a1'},{name:'a2'}]}"));
        assertNull(p.applyCatalog(catalog("{revision:1,commands:[{name:'old'}]}")));
        p.applyCatalog(catalog("{revision:3,commands:[{name:'a2',installationId:'new'},{name:'a3'}]}"));
        assertEquals(Set.of("a2", "a3"), p.snapshot().byName().keySet());
        assertEquals("new", p.snapshot().byName().get("a2").get(0).installationId());
        assertFalse(p.claim("/a1", "c", CALLER));
        assertTrue(p.claim("/a3", "c", CALLER));
    }

    @Test
    void inFlightInvocationKeepsTheSnapshotItWasClaimedWith() throws Exception {
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicReference<JsonObject> seen = new AtomicReference<>();
        ExecutorService pool = Executors.newFixedThreadPool(2);
        CommandProbe p = new CommandProbe(NATIVE::contains, inv -> {
            seen.set(inv);
            entered.countDown();
            release.await(5, TimeUnit.SECONDS);
            return new CommandProbe.Outcome("ok", List.of("done"));
        }, (conn, text) -> replies.add(text), receipts::add, pool);
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe',installationId:'i1'}]}"));
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertTrue(entered.await(5, TimeUnit.SECONDS));
        p.applyCatalog(catalog("{revision:2,commands:[]}"));
        release.countDown();
        pool.shutdown();
        assertTrue(pool.awaitTermination(5, TimeUnit.SECONDS));
        assertEquals("i1", seen.get().getAsJsonArray("installations").get(0).getAsString());
        assertEquals(1L, receipts.get(0).get("catalogRevision").getAsLong());
        assertFalse(p.claim("/tprobe", "c", CALLER));
    }

    @Test
    void malformedCatalogCountsAsOfflineAndKeepsTheSnapshot() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertNull(p.applyCatalog(catalog("{revision:'not a number',commands:[]}")));
        assertFalse(p.online());
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertEquals(List.of("c:" + CommandProbe.UNAVAILABLE), replies);
    }

    @Test
    void splitScreenCallerIsRefusedPrivately() {
        CommandProbe p = probe(ok());
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertTrue(p.claim("/tprobe", "c", new CommandProbe.Caller("tester", 1L, "user", 2, 7L)));
        assertTrue(sent.isEmpty());
        assertEquals(List.of("c:" + CommandProbe.AMBIGUOUS), replies);
    }

    @Test
    void fullWorkerPoolFailsPrivatelyInsteadOfQueueing() {
        ExecutorService full = new DirectExecutor() {
            @Override public void execute(Runnable r) {
                throw new java.util.concurrent.RejectedExecutionException("full");
            }
        };
        CommandProbe p = new CommandProbe(NATIVE::contains, ok(), (conn, text) -> replies.add(conn + ":" + text),
                receipts::add, full);
        p.applyCatalog(catalog("{revision:1,commands:[{name:'tprobe'}]}"));
        assertTrue(p.claim("/tprobe", "c", CALLER));
        assertTrue(sent.isEmpty());
        assertEquals(List.of("c:" + CommandProbe.BUSY), replies);
    }

    @Test
    void commandNameAndArguments() {
        assertEquals("x", CommandProbe.commandName("/x"));
        assertEquals("x", CommandProbe.commandName("/  x  y"));
        assertNull(CommandProbe.commandName("x"));
        assertEquals(List.of("a", "b c", ""), CommandProbe.arguments("/x a \"b c\" \"\""));
        assertNull(CommandProbe.arguments("/x a \"b"));
    }

    /** Runs tasks inline so assertions see the reply without waiting. */
    private static class DirectExecutor extends java.util.concurrent.AbstractExecutorService {
        @Override public void execute(Runnable r) { r.run(); }
        @Override public void shutdown() { }
        @Override public List<Runnable> shutdownNow() { return List.of(); }
        @Override public boolean isShutdown() { return false; }
        @Override public boolean isTerminated() { return false; }
        @Override public boolean awaitTermination(long t, TimeUnit u) { return true; }
    }
}
