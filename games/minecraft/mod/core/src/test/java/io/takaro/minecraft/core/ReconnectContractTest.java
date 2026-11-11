package io.takaro.minecraft.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BooleanSupplier;

import static org.junit.jupiter.api.Assertions.*;

/** The reconnect behaviour of one client against a fake Takaro (gettakaro/connectors#858). */
class ReconnectContractTest {

    /** Accepts every identify; answers pings unless told to go silent. */
    static final class FakeTakaro extends WebSocketServer {
        final List<Long> identifiedAt = new CopyOnWriteArrayList<>();
        final List<WebSocket> conns = new CopyOnWriteArrayList<>();
        volatile int pings;
        volatile boolean silent;

        FakeTakaro(int port) {
            super(new InetSocketAddress("127.0.0.1", port));
            setReuseAddr(true);
        }

        @Override public void onOpen(WebSocket conn, ClientHandshake handshake) { conns.add(conn); }
        @Override public void onClose(WebSocket conn, int code, String reason, boolean remote) {}
        @Override public void onError(WebSocket conn, Exception ex) {}
        @Override public void onStart() {}

        @Override
        public void onMessage(WebSocket conn, String message) {
            JsonObject msg = JsonParser.parseString(message).getAsJsonObject();
            String type = msg.get("type").getAsString();
            if ("ping".equals(type)) {
                pings++;
                if (!silent) conn.send("{\"type\":\"pong\"}");
            } else if ("identify".equals(type) && !silent) {
                identifiedAt.add(System.currentTimeMillis());
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs-1\"}}}");
            }
        }
    }

    private FakeTakaro takaro;
    private TakaroWebSocketClient client;
    private ServerSocket mute;
    private final TakaroConnectorTestSupport.LogAdapter adapter = new TakaroConnectorTestSupport.LogAdapter();

    @AfterEach
    void stop() throws Exception {
        if (client != null) client.shutdown();
        if (takaro != null) takaro.stop(1000);
        if (mute != null) mute.close();
    }

    private static int freePort() throws Exception {
        try (ServerSocket s = new ServerSocket(0)) {
            return s.getLocalPort();
        }
    }

    private static void await(String what, long millis, BooleanSupplier cond) throws InterruptedException {
        long deadline = System.currentTimeMillis() + millis;
        while (!cond.getAsBoolean()) {
            if (System.currentTimeMillis() > deadline) fail("timed out waiting for " + what);
            Thread.sleep(20);
        }
    }

    private TakaroWebSocketClient start(int port, ReconnectPolicy policy) throws Exception {
        TakaroConfig config = new TakaroConfig();
        config.setIdentityToken("id");
        config.setRegistrationToken("good");
        client = new TakaroWebSocketClient(new URI("ws://127.0.0.1:" + port + "/"), adapter, config, "config",
                new EventOutbox(), null, null, policy);
        return client;
    }

    @Test
    void aStableSessionThatDropsIsIdentifiedAgainWithinASecond() throws Exception {
        int port = freePort();
        takaro = new FakeTakaro(port);
        takaro.start();
        start(port, new ReconnectPolicy(300)).connect();
        await("first identify", 5000, () -> takaro.identifiedAt.size() == 1);
        Thread.sleep(400); // past the stable threshold

        for (int drop = 1; drop <= 3; drop++) {
            int before = takaro.identifiedAt.size();
            long cut = System.currentTimeMillis();
            takaro.conns.get(takaro.conns.size() - 1).closeConnection(1006, "reset");
            await("re-identify " + drop, 5000, () -> takaro.identifiedAt.size() == before + 1);
            long gap = takaro.identifiedAt.get(before) - cut;
            assertTrue(gap < 1000, "drop " + drop + " took " + gap + " ms");
            Thread.sleep(400);
        }
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("(fast retry)")), adapter.infos.toString());
    }

    @Test
    void pingsAreAnsweredAndPongIsNotAnUnknownMessage() throws Exception {
        int port = freePort();
        takaro = new FakeTakaro(port);
        takaro.start();
        TakaroWebSocketClient c = start(port, new ReconnectPolicy());
        c.setPingIntervalMillis(200);
        c.connect();
        await("pings", 5000, () -> takaro.pings >= 2);
        Thread.sleep(200);
        assertTrue(adapter.warnings.stream().noneMatch(l -> l.contains("Unknown message type")), adapter.warnings.toString());
        assertEquals(1, takaro.identifiedAt.size(), "an answered ping keeps the connection");
    }

    @Test
    void aSilentLinkIsDroppedAfterTheInboundTimeoutAndReconnectsFast() throws Exception {
        int port = freePort();
        takaro = new FakeTakaro(port);
        takaro.start();
        TakaroWebSocketClient c = start(port, new ReconnectPolicy(300));
        c.setPingIntervalMillis(300);
        c.setSilenceLimitMillis(1500);
        c.connect();
        await("first identify", 5000, () -> takaro.identifiedAt.size() == 1);
        Thread.sleep(400);
        takaro.silent = true; // a blackhole: the socket stays open, nothing comes back
        long cut = System.currentTimeMillis();
        await("dead link noticed", 6000, () -> adapter.warnings.stream().anyMatch(l -> l.contains("treating the connection as dead")));
        long noticed = System.currentTimeMillis() - cut;
        assertTrue(noticed >= 1000 && noticed < 4000, "noticed after " + noticed + " ms");
        takaro.silent = false;
        await("re-identify", 3000, () -> takaro.identifiedAt.size() == 2);
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("(fast retry)")), adapter.infos.toString());
    }

    @Test
    void aHandshakeThatNeverCompletesIsAbandonedAndRetried() throws Exception {
        mute = new ServerSocket(0); // accepts TCP, never answers the upgrade
        List<Socket> accepted = new CopyOnWriteArrayList<>();
        Thread acceptor = new Thread(() -> {
            try {
                while (true) accepted.add(mute.accept());
            } catch (Exception ignored) {
            }
        });
        acceptor.setDaemon(true);
        acceptor.start();
        TakaroWebSocketClient c = start(mute.getLocalPort(), new ReconnectPolicy());
        c.setHandshakeTimeoutMillis(1000);
        c.connect();
        await("abandoned", 6000, () -> adapter.warnings.stream().anyMatch(l -> l.contains("abandoning the attempt")));
        await("second attempt", 6000, () -> accepted.size() >= 2);
    }

    @Test
    void refusedConnectionsBackOffAndStopGrowingAtTheCap() throws Exception {
        int port = freePort(); // nothing listens
        TakaroConfig config = new TakaroConfig();
        ReconnectPolicy policy = new ReconnectPolicy();
        long previousCeiling = 0;
        for (int i = 0; i < 12; i++) {
            long d = policy.onConnectionEnded(-1, config.getReconnectDelay(), config.getBackoffMultiplier(),
                    config.getMaxReconnectDelay(), 0.999999);
            assertTrue(d <= 60_000, "attempt " + i + " waited " + d);
            assertTrue(d >= previousCeiling / 2, "attempt " + i + " shrank to " + d);
            previousCeiling = d;
        }
        assertTrue(previousCeiling > 59_000, "the cap is reached: " + previousCeiling);
        start(port, new ReconnectPolicy()).connect();
        await("retry scheduled", 5000, () -> adapter.infos.stream().anyMatch(l -> l.contains("Reconnecting in")));
    }
}
