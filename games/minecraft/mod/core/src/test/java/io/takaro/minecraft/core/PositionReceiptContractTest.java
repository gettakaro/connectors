package io.takaro.minecraft.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.model.PlayerInfo;
import io.takaro.minecraft.core.model.PlayerLocation;
import io.takaro.minecraft.core.model.PositionSample;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import java.net.InetSocketAddress;
import java.net.URI;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;

import static org.junit.jupiter.api.Assertions.*;

/** Real Java-WebSocket writes and receipts, with a deterministic position clock. */
class PositionReceiptContractTest {
    private final AtomicLong clock = new AtomicLong(TimeUnit.SECONDS.toNanos(100));
    private final AtomicInteger received = new AtomicInteger();
    private final AtomicInteger reads = new AtomicInteger();
    private final AtomicInteger x = new AtomicInteger(1);
    private final EventOutbox outbox = new EventOutbox();
    private final TakaroConnectorTestSupport.LogAdapter adapter = new TakaroConnectorTestSupport.LogAdapter() {
        @Override public List<PlayerInfo> getPlayers() { return List.of(PlayerInfo.of("u1", "Steve", "", 0)); }
        @Override public PlayerLocation getPlayerLocation(String gameId) {
            reads.incrementAndGet();
            return new PlayerLocation(x.get(), 64, 3, "overworld");
        }
    };
    private final PositionPublisher publisher = new PositionPublisher(clock::get);
    private FakeTakaro takaro;
    private TakaroWebSocketClient client;

    @AfterEach
    void stop() throws Exception {
        if (client != null) client.shutdown();
        if (takaro != null) takaro.stop(1000);
    }

    private void start(boolean reconnect) throws Exception {
        takaro = new FakeTakaro();
        takaro.start();
        await(() -> takaro.ready);
        TakaroConfig config = new TakaroConfig();
        config.setIdentityToken("id");
        config.setRegistrationToken("reg");
        config.setReconnectEnabled(reconnect);
        client = new TakaroWebSocketClient(new URI("ws://127.0.0.1:" + takaro.getPort()), adapter, config,
                "config", outbox, null, null, new ReconnectPolicy(0), clock::get) {
            @Override public void onMessage(String text) {
                super.onMessage(text);
                received.incrementAndGet();
            }
        };
        client.connect();
        await(client::isV1);
        assertFalse(client.acceptsPlayerPositions(), "a new connection waits the declared interval");
        clock.addAndGet(TimeUnit.SECONDS.toNanos(3));
    }

    @Test
    void delayedWrongFutureAndDuplicateReceiptsCannotCompressFreshCaptures() throws Exception {
        start(false);
        publisher.capture(adapter, client);
        await(() -> takaro.positions.size() == 1);
        JsonObject first = takaro.positions.get(0);
        String stream = first.get("streamId").getAsString();
        assertNotEquals(outbox.streamId(), stream);
        assertEquals(0, outbox.size());
        clock.addAndGet(TimeUnit.SECONDS.toNanos(10));
        x.set(4);
        publisher.capture(adapter, client);
        assertEquals(1, reads.get(), "no stale sample is captured while a receipt is pending");
        client.emitLog("gameplay continues");
        await(() -> takaro.gameplay.size() == 1);
        assertEquals(1, outbox.size(), "gameplay has its independent acknowledgement stream");
        receipt("wrong-stream", 1);
        receipt(stream, 2);
        assertFalse(client.acceptsPlayerPositions());
        receipt(stream, 1);
        clock.addAndGet(TimeUnit.SECONDS.toNanos(3) - 1);
        publisher.capture(adapter, client);
        assertEquals(1, reads.get());
        clock.incrementAndGet();
        receipt(stream, 1);
        x.set(9);
        publisher.capture(adapter, client);
        await(() -> takaro.positions.size() == 2);
        JsonObject second = takaro.positions.get(1);
        assertEquals(2, reads.get());
        assertEquals(2, second.get("seq").getAsLong());
        assertEquals(9, positionX(second));
        assertEquals(1, outbox.size(), "position receipts do not acknowledge gameplay");
    }

    @Test
    void missingReceiptReconnectsAndRejectsTheOldStreamAndCapture() throws Exception {
        start(true);
        long oldCapture = clock.get();
        publisher.capture(adapter, client);
        await(() -> takaro.positions.size() == 1);
        String oldStream = takaro.positions.get(0).get("streamId").getAsString();
        clock.addAndGet(TimeUnit.SECONDS.toNanos(30));
        client.checkSilence();
        await(() -> takaro.identifies.get() >= 2 && client.isV1());
        assertTrue(adapter.warnings.stream().anyMatch(s -> s.contains("No position acknowledgement")));
        assertEquals(1, takaro.positions.size(), "reconnect must not replay transient snapshots");
        clock.addAndGet(TimeUnit.SECONDS.toNanos(3));
        client.emitPlayerPositions(List.of(new PositionSample("u1", new PlayerLocation(1, 64, 3, "overworld"))), oldCapture);
        client.emitPlayerPositions(List.of(new PositionSample("u1", new PlayerLocation(1, 64, 3, "overworld"))), clock.get() + 1);
        assertTrue(client.acceptsPlayerPositions(), "old and future captures cannot consume the new slot");
        x.set(12);
        publisher.capture(adapter, client);
        await(() -> takaro.positions.size() == 2);
        JsonObject fresh = takaro.positions.get(1);
        assertNotEquals(oldStream, fresh.get("streamId").getAsString());
        assertEquals(1, fresh.get("seq").getAsLong());
        assertEquals(12, positionX(fresh));
        receipt(oldStream, 1);
        assertFalse(client.acceptsPlayerPositions(), "old-stream receipts cannot release the current snapshot");
    }

    @Test
    void rejectionReleasesOnlyItsMatchingSnapshotAndStillWaitsTheInterval() throws Exception {
        start(false);
        publisher.capture(adapter, client);
        await(() -> takaro.positions.size() == 1);
        JsonObject first = takaro.positions.get(0);
        String stream = first.get("streamId").getAsString();
        int before = received.get();
        takaro.connection.send("{\"type\":\"eventError\",\"payload\":{\"streamId\":\"" + stream
                + "\",\"seq\":1},\"error\":{\"code\":\"rate_limited\",\"message\":\"test rejection\"}}");
        await(() -> received.get() > before && !adapter.warnings.isEmpty());
        assertFalse(client.acceptsPlayerPositions());
        clock.addAndGet(TimeUnit.SECONDS.toNanos(3));
        assertTrue(client.acceptsPlayerPositions());
        assertTrue(adapter.warnings.stream().anyMatch(s -> s.contains("rate_limited test rejection")));
    }

    private void receipt(String stream, long seq) throws Exception {
        int before = received.get();
        takaro.connection.send("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"" + stream + "\",\"upTo\":" + seq + "}}");
        await(() -> received.get() > before);
    }

    private static double positionX(JsonObject payload) {
        return payload.getAsJsonObject("data").getAsJsonArray("positions").get(0).getAsJsonObject()
                .getAsJsonObject("position").get("x").getAsDouble();
    }

    private static void await(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(6);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) fail("timed out waiting for fake Takaro");
            Thread.sleep(10);
        }
    }

    private static final class FakeTakaro extends WebSocketServer {
        volatile boolean ready;
        volatile WebSocket connection;
        final AtomicInteger identifies = new AtomicInteger();
        final List<JsonObject> positions = new CopyOnWriteArrayList<>();
        final List<JsonObject> gameplay = new CopyOnWriteArrayList<>();

        FakeTakaro() { super(new InetSocketAddress("127.0.0.1", 0)); }
        @Override public void onStart() { ready = true; }
        @Override public void onOpen(WebSocket conn, ClientHandshake handshake) { connection = conn; }
        @Override public void onClose(WebSocket conn, int code, String reason, boolean remote) {}
        @Override public void onError(WebSocket conn, Exception error) {}
        @Override public void onMessage(WebSocket conn, String text) {
            JsonObject message = JsonParser.parseString(text).getAsJsonObject();
            switch (message.get("type").getAsString()) {
                case "identify" -> {
                    identifies.incrementAndGet();
                    conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"test\",\"protocolVersion\":1}}");
                }
                case "ping" -> conn.send("{\"type\":\"pong\"}");
                case "gameEvent" -> {
                    JsonObject payload = message.getAsJsonObject("payload");
                    (payload.get("type").getAsString().equals("player-positions") ? positions : gameplay).add(payload);
                }
                default -> { }
            }
        }
    }
}
