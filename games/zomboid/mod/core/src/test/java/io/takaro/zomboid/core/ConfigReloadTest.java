package io.takaro.zomboid.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.java_websocket.WebSocket;
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
 * A fake Takaro that accepts one registration token: a rejected token logs the banner,
 * and a corrected token reaches Takaro without waiting for the backoff.
 */
class ConfigReloadTest {

    private FakeTakaro takaro;
    private final List<String> logs = new CopyOnWriteArrayList<>();
    private TakaroConnector connector;

    @BeforeEach
    void start() throws Exception {
        takaro = new FakeTakaro("good-token");
        takaro.start();
        assertTrue(takaro.ready(), "fake Takaro did not start");
    }

    @AfterEach
    void stop() throws Exception {
        if (connector != null) {
            connector.shutdown();
        }
        takaro.stop(1000);
    }

    private TakaroConfig config(String token) {
        TakaroConfig c = new TakaroConfig();
        c.setWsUrl("ws://127.0.0.1:" + takaro.getPort() + "/");
        c.setIdentityToken("my-server");
        c.setRegistrationToken(token);
        c.setConfigFileHint("/x/TakaroConfig.txt");
        // A long backoff: only a config change can make the next attempt come quickly.
        c.setReconnectDelay(60_000);
        return c;
    }

    @Test
    void rejectedTokenThenCorrectedTokenReconnectsAtOnce() throws Exception {
        TakaroConfig shared = config("bad-token");
        connector = new TakaroConnector(new LogAdapter(logs), shared);
        connector.connect();

        waitFor(() -> logs.stream().anyMatch(l -> l.contains("Takaro rejected identify: Invalid registrationToken provided (BadRequestError, http 400).")));
        assertTrue(logs.stream().anyMatch(l -> l.contains("Check registrationToken in /x/TakaroConfig.txt")));
        waitFor(() -> logs.stream().anyMatch(l -> l.contains("Reconnecting in 60s")));

        connector.applyConfig(config("good-token"));

        waitFor(() -> logs.stream().anyMatch(l -> l.contains("Identified successfully")));
        assertTrue(logs.stream().anyMatch(l -> l.contains("Config changed; reconnecting")));
        assertEquals(List.of("bad-token", "good-token"), takaro.tokensSeen);
    }

    @Test
    void conflictGetsItsOwnBannerAndTheRawErrorIsNeverLogged() throws Exception {
        TakaroConfig c = config("conflict-token");
        c.setDebugEnabled(true);
        connector = new TakaroConnector(new LogAdapter(logs), c);
        connector.connect();

        waitFor(() -> logs.stream().anyMatch(l -> l.contains("Conflict (ConflictError, http 409)")));
        assertTrue(logs.stream().anyMatch(l -> l.contains("Put a new value after identityToken=")));
        assertTrue(logs.stream().noneMatch(l -> l.contains("SECRET-JWT")), "the raw error must not be logged");
    }

    @Test
    void unchangedConnectionSettingsDoNotReconnect() throws Exception {
        TakaroConfig shared = config("good-token");
        connector = new TakaroConnector(new LogAdapter(logs), shared);
        connector.connect();
        waitFor(() -> logs.stream().anyMatch(l -> l.contains("Identified successfully")));

        TakaroConfig same = config("good-token");
        same.setDebugEnabled(true);
        connector.applyConfig(same);
        Thread.sleep(300);

        assertTrue(shared.isDebugEnabled(), "the other settings still apply");
        assertEquals(1, takaro.tokensSeen.size());
    }

    @Test
    void replacedClientDoesNotReconnectBehindTheNewOne() throws Exception {
        connector = new TakaroConnector(new LogAdapter(logs), config("good-token"));
        connector.connect();
        waitFor(() -> takaro.tokensSeen.size() == 1);
        TakaroConfig changed = config("good-token");
        changed.setIdentityToken("other-server");
        changed.setReconnectDelay(100);
        connector.applyConfig(changed);
        waitFor(() -> takaro.tokensSeen.size() == 2);

        Thread.sleep(800);
        assertEquals(2, takaro.tokensSeen.size(), "the old socket's close must not schedule a reconnect");
        assertEquals(1, takaro.getConnections().size());
    }

    private static void waitFor(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() > deadline) {
                fail("condition not met within 10 s");
            }
            Thread.sleep(25);
        }
    }

    private static final class FakeTakaro extends WebSocketServer {
        private final String goodToken;
        final List<String> tokensSeen = new CopyOnWriteArrayList<>();
        private volatile boolean started;

        FakeTakaro(String goodToken) {
            super(new InetSocketAddress("127.0.0.1", 0));
            this.goodToken = goodToken;
            setReuseAddr(true);
        }

        boolean ready() throws InterruptedException {
            for (int i = 0; i < 200 && !started; i++) {
                Thread.sleep(25);
            }
            return started;
        }

        @Override public void onStart() { started = true; }
        @Override public void onOpen(WebSocket conn, ClientHandshake handshake) {}
        @Override public void onClose(WebSocket conn, int code, String reason, boolean remote) {}
        @Override public void onError(WebSocket conn, Exception ex) {}

        @Override
        public void onMessage(WebSocket conn, String message) {
            JsonObject json = JsonParser.parseString(message).getAsJsonObject();
            if (!"identify".equals(json.get("type").getAsString())) {
                return;
            }
            String token = json.getAsJsonObject("payload").get("registrationToken").getAsString();
            tokensSeen.add(token);
            if ("conflict-token".equals(token)) {
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"ConflictError\","
                        + "\"message\":\"Conflict\",\"http\":409,\"request\":{\"headers\":{\"x-takaro-token\":\"SECRET-JWT\"}}}}}");
            } else if (goodToken.equals(token)) {
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs-1\"}}}");
            } else {
                // What connect.takaro.io answers, and it keeps the socket open afterwards.
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"BadRequestError\","
                        + "\"message\":\"Invalid registrationToken provided\",\"http\":400}}}");
            }
        }
    }

    private static final class LogAdapter implements GameAdapter {
        private final List<String> logs;
        LogAdapter(List<String> logs) { this.logs = logs; }
        @Override public void logInfo(String msg) { logs.add(msg); }
        @Override public void logWarning(String msg) { logs.add(msg); }
        @Override public void logDebug(String msg) { logs.add(msg); }
        @Override public void runOnMainThread(Runnable task) { task.run(); }
        @Override public PlayerInfo getPlayer(String gameId) { return null; }
        @Override public List<PlayerInfo> getPlayers() { return Collections.emptyList(); }
        @Override public PlayerLocation getPlayerLocation(String gameId) { return null; }
        @Override public List<InventoryItem> getPlayerInventory(String gameId) { return Collections.emptyList(); }
        @Override public List<GameItem> listItems() { return Collections.emptyList(); }
        @Override public List<GameEntity> listEntities() { return Collections.emptyList(); }
        @Override public List<GameLocation> listLocations() { return Collections.emptyList(); }
        @Override public void giveItem(String gameId, String itemCode, int amount, String quality) {}
        @Override public void sendMessage(String message, String recipientGameId) {}
        @Override public CommandResult executeConsoleCommand(String command) { return new CommandResult(true, "", null); }
        @Override public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {}
        @Override public void kickPlayer(String gameId, String reason) {}
        @Override public void banPlayer(String gameId, String reason, String expiresAt) {}
        @Override public void unbanPlayer(String gameId) {}
        @Override public List<BanEntry> listBans() { return Collections.emptyList(); }
        @Override public void shutdownServer() {}
        @Override public void setEventEmitter(EventEmitter emitter) {}
    }
}
