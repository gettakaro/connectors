package io.takaro.zomboid.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.java_websocket.WebSocket;
import org.java_websocket.framing.CloseFrame;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.net.InetSocketAddress;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

import static org.junit.jupiter.api.Assertions.*;

/**
 * A real socket against a fake Takaro: a stable session is dropped mid-stream, the connector is
 * back within 2 s, and neither the event in flight (sent, never acknowledged) nor the event
 * produced while the socket was down is lost.
 */
class ReconnectContractTest {

    private FakeTakaro takaro;
    private TakaroConnector connector;
    private final List<String> logs = new CopyOnWriteArrayList<>();
    private volatile EventEmitter emitter;

    @BeforeEach
    void start() throws Exception {
        takaro = new FakeTakaro();
        takaro.start();
        assertTrue(takaro.ready(), "fake Takaro did not start");
    }

    @AfterEach
    void stop() throws Exception {
        if (connector != null) connector.shutdown();
        takaro.stop(1000);
    }

    @Test
    void droppedStableSessionReconnectsFastAndKeepsEveryEvent() throws Exception {
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:" + takaro.getPort() + "/");
        config.setIdentityToken("my-server");
        config.setRegistrationToken("good-token");
        connector = new TakaroConnector(new Adapter(), config);
        connector.connect();
        waitFor(() -> takaro.identifies.size() == 1, 10);

        emitter.emitLog("before");
        waitFor(() -> takaro.messages.contains("before"), 5);

        Thread.sleep(ReconnectPolicy.STABLE_MS + 300);
        takaro.ackEvents = false;
        emitter.emitLog("in-flight");
        waitFor(() -> takaro.messages.contains("in-flight"), 5);
        long dropped = System.nanoTime();
        takaro.dropAll();
        waitFor(() -> logs.stream().anyMatch(l -> l.contains("fast retry")), 5);
        emitter.emitLog("during-gap");

        takaro.ackEvents = true;
        waitFor(() -> takaro.identifies.size() == 2, 5);
        double seconds = (System.nanoTime() - dropped) / 1e9;
        assertTrue(seconds <= 2.0, "identified again after " + seconds + " s");

        waitFor(() -> takaro.messages.stream().filter("in-flight"::equals).count() == 2
                && takaro.messages.contains("during-gap"), 5);
        assertEquals(1, takaro.messages.stream().filter("before"::equals).count(), "an acknowledged event is not resent");
        assertEquals(List.of(1L, 2L, 2L, 3L), takaro.seqs);
    }

    private static void waitFor(BooleanSupplier condition, int seconds) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() > deadline) fail("condition not met within " + seconds + " s");
            Thread.sleep(10);
        }
    }

    private static final class FakeTakaro extends WebSocketServer {
        final List<String> identifies = new CopyOnWriteArrayList<>();
        final List<String> messages = new CopyOnWriteArrayList<>();
        final List<Long> seqs = new CopyOnWriteArrayList<>();
        volatile boolean ackEvents = true;
        private volatile boolean started;

        FakeTakaro() {
            super(new InetSocketAddress("127.0.0.1", 0));
            setReuseAddr(true);
        }

        boolean ready() throws InterruptedException {
            for (int i = 0; i < 200 && !started; i++) Thread.sleep(25);
            return started;
        }

        /** What a mid-path RST looks like to the connector: the socket just ends, no close frame. */
        void dropAll() {
            for (WebSocket c : getConnections()) c.closeConnection(CloseFrame.ABNORMAL_CLOSE, "dropped");
        }

        @Override public void onStart() { started = true; }
        @Override public void onOpen(WebSocket conn, ClientHandshake handshake) {}
        @Override public void onClose(WebSocket conn, int code, String reason, boolean remote) {}
        @Override public void onError(WebSocket conn, Exception ex) {}

        @Override
        public void onMessage(WebSocket conn, String message) {
            JsonObject json = JsonParser.parseString(message).getAsJsonObject();
            String type = json.get("type").getAsString();
            if ("identify".equals(type)) {
                identifies.add(message);
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs-1\"},\"protocolVersion\":1}}");
            } else if ("gameEvent".equals(type)) {
                JsonObject payload = json.getAsJsonObject("payload");
                String streamId = payload.get("streamId").getAsString();
                long seq = payload.get("seq").getAsLong();
                seqs.add(seq);
                messages.add(payload.getAsJsonObject("data").get("msg").getAsString());
                if (ackEvents) {
                    conn.send("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"" + streamId + "\",\"upTo\":" + seq + "}}");
                }
            }
        }
    }

    private final class Adapter implements GameAdapter {
        @Override public void logInfo(String msg) { logs.add(msg); }
        @Override public void logWarning(String msg) { logs.add(msg); }
        @Override public void runOnMainThread(Runnable task) { task.run(); }
        @Override public PlayerInfo getPlayer(String gameId) { return null; }
        @Override public List<PlayerInfo> getPlayers() { return Collections.emptyList(); }
        @Override public PlayerLocation getPlayerLocation(String gameId) { return null; }
        @Override public List<InventoryItem> getPlayerInventory(String gameId) { return Collections.emptyList(); }
        @Override public List<GameItem> listItems() { return Collections.emptyList(); }
        @Override public List<GameEntity> listEntities() { return Collections.emptyList(); }
        @Override public List<GameLocation> listLocations() { return Collections.emptyList(); }
        @Override public void giveItem(String gameId, String itemCode, int amount, String quality) {}
        @Override public void sendMessage(String message, String senderName, String recipientGameId) {}
        @Override public CommandResult executeConsoleCommand(String command) { return new CommandResult(true, "", null); }
        @Override public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {}
        @Override public void kickPlayer(String gameId, String reason) {}
        @Override public void banPlayer(String gameId, String reason, String expiresAt) {}
        @Override public void unbanPlayer(String gameId) {}
        @Override public List<BanEntry> listBans() { return Collections.emptyList(); }
        @Override public void shutdownServer() {}
        @Override public void setEventEmitter(EventEmitter e) { emitter = e; }
    }
}
