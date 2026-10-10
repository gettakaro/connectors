package io.takaro.minecraft.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.config.ConfigFile;
import io.takaro.minecraft.core.config.PropertiesConfigFormat;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BooleanSupplier;

import static org.junit.jupiter.api.Assertions.*;

/** The connector against a fake Takaro: token changes in the file apply without a restart. */
class ConfigReloadContractTest {

    @TempDir
    Path dir;

    private FakeTakaro takaro;
    private TakaroConnector connector;

    /** Accepts identify only with registration token "good". */
    static final class FakeTakaro extends WebSocketServer {
        final List<JsonObject> identifies = new CopyOnWriteArrayList<>();

        FakeTakaro(int port) {
            super(new InetSocketAddress("127.0.0.1", port));
            setReuseAddr(true);
        }

        @Override public void onOpen(WebSocket conn, ClientHandshake handshake) {}
        @Override public void onClose(WebSocket conn, int code, String reason, boolean remote) {}
        @Override public void onError(WebSocket conn, Exception ex) {}
        @Override public void onStart() {}

        @Override
        public void onMessage(WebSocket conn, String message) {
            JsonObject msg = JsonParser.parseString(message).getAsJsonObject();
            if (!"identify".equals(msg.get("type").getAsString())) return;
            JsonObject payload = msg.getAsJsonObject("payload");
            identifies.add(payload);
            String token = payload.get("registrationToken").getAsString();
            if ("leaky".equals(token)) {
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"BadRequestError\","
                        + "\"message\":\"Invalid registrationToken provided\",\"http\":400,"
                        + "\"request\":{\"headers\":{\"x-takaro-token\":\"SECRET-JWT\"}}}}}");
                return;
            }
            if ("conflict".equals(token)) {
                conn.send("{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"ConflictError\","
                        + "\"message\":\"Unique constraint violation\",\"http\":409}}}");
                return;
            }
            boolean ok = "good".equals(token);
            conn.send(ok
                    ? "{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs-1\"}}}"
                    : "{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"message\":\"Invalid registrationToken provided\"}}}");
        }
    }

    private static int freePort() throws Exception {
        try (ServerSocket s = new ServerSocket(0)) {
            return s.getLocalPort();
        }
    }

    private static void await(String what, BooleanSupplier condition) throws InterruptedException {
        long deadline = System.currentTimeMillis() + 10_000;
        while (!condition.getAsBoolean()) {
            if (System.currentTimeMillis() > deadline) fail("timed out waiting for " + what);
            Thread.sleep(20);
        }
    }

    private TakaroConnectorTestSupport.LogAdapter adapter;
    private Path file;

    private void startConnector(String token) throws Exception {
        int port = freePort();
        takaro = new FakeTakaro(port);
        takaro.start();
        file = dir.resolve("takaro.properties");
        Files.writeString(file, "takaro.websocket.url=ws://127.0.0.1:" + port + "/\n"
                + "takaro.authentication.identity_token=\n"
                + "takaro.authentication.registration_token=" + token + "\n");
        adapter = new TakaroConnectorTestSupport.LogAdapter();
        ConfigFile cf = new ConfigFile(file, new PropertiesConfigFormat(), adapter, Map.<String, String>of()::get,
                () -> "uuid-1");
        connector = new TakaroConnector(adapter, cf, Optional.empty());
        connector.start();
    }

    private void saveToken(String token) throws Exception {
        String text = Files.readString(file).replaceAll("(?m)^takaro\\.authentication\\.registration_token=.*$",
                "takaro.authentication.registration_token=" + token);
        Files.writeString(file, text);
        connector.checkConfig(); // the watcher tick, without waiting 5 s
    }

    @AfterEach
    void stop() throws Exception {
        if (connector != null) connector.shutdown();
        if (takaro != null) takaro.stop(1000);
    }

    @Test
    void noTokenBannerThenPastedTokenConnectsWithTheGeneratedIdentity() throws Exception {
        startConnector("");
        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("registration_token not set")));
        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains(file.toAbsolutePath().toString())),
                "the banner names the file to edit");
        Thread.sleep(300);
        assertEquals(0, takaro.identifies.size(), "no token, no connection");

        saveToken("good");
        await("identified", () -> adapter.infos.stream().anyMatch(l -> l.contains("Identified successfully, server ID: gs-1")));
        assertEquals("uuid-1", takaro.identifies.get(0).get("identityToken").getAsString());
        assertTrue(Files.readString(file).contains("identity_token=uuid-1"));
    }

    @Test
    void rejectedTokenBannerThenCorrectedTokenReconnectsAtOnce() throws Exception {
        startConnector("wrong");
        await("rejection", () -> adapter.warnings.stream().anyMatch(l -> l.contains("Identify failed: Invalid registrationToken provided")));
        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("Saving a corrected")));

        saveToken("good");
        await("identified", () -> adapter.infos.stream().anyMatch(l -> l.contains("Identified successfully")));
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("changed; reconnecting with the new settings")));
        JsonObject last = takaro.identifies.get(takaro.identifies.size() - 1);
        assertEquals("good", last.get("registrationToken").getAsString());
        assertEquals("uuid-1", last.get("identityToken").getAsString(), "a token change keeps the identity");
    }

    @Test
    void unrelatedSaveDoesNotReconnect() throws Exception {
        startConnector("good");
        await("identified", () -> adapter.infos.stream().anyMatch(l -> l.contains("Identified successfully")));
        Files.writeString(file, Files.readString(file) + "# a comment\n");
        connector.checkConfig();
        Files.writeString(file, Files.readString(file) + "takaro.debug=true\n");
        connector.checkConfig();
        Thread.sleep(300);
        assertEquals(1, takaro.identifies.size());
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("the connection is unchanged")));
    }

    @Test
    void shutdownStopsWatchingAndConnecting() throws Exception {
        startConnector("wrong");
        await("rejection", () -> adapter.infos.stream().anyMatch(l -> l.contains("Reconnecting in")));
        // between reconnect attempts: closeBlocking() used to wait here forever
        assertTimeoutPreemptively(java.time.Duration.ofSeconds(5), connector::shutdown);
        int seen = takaro.identifies.size();
        saveToken("good");
        Thread.sleep(500);
        assertEquals(seen, takaro.identifies.size(), "no connection after shutdown");
    }

    @Test
    void concurrentConfigChangesAndShutdownLeaveNoSocketBehind() throws Exception {
        startConnector("good");
        await("identified", () -> !takaro.identifies.isEmpty());
        Thread[] writers = new Thread[4];
        for (int i = 0; i < writers.length; i++) {
            final int n = i;
            writers[i] = new Thread(() -> {
                for (int j = 0; j < 5; j++) {
                    TakaroConfig c = new TakaroConfig();
                    c.setWsUrl("ws://127.0.0.1:" + takaro.getPort() + "/");
                    c.setIdentityToken("uuid-1");
                    c.setRegistrationToken("good-" + n + "-" + j);
                    connector.applyConfig(c);
                }
            });
            writers[i].start();
        }
        connector.shutdown();
        for (Thread t : writers) t.join();
        Thread.sleep(500);
        assertEquals(0, takaro.getConnections().size(), "every socket is closed after shutdown");
    }

    @Test
    void identifyErrorsNeverLogTakarosInternalRequest() throws Exception {
        startConnector("leaky");
        await("rejection", () -> adapter.warnings.stream().anyMatch(l -> l.contains("Identify failed: Invalid registrationToken provided (HTTP 400)")));
        assertTrue(adapter.warnings.stream().noneMatch(l -> l.contains("SECRET-JWT")));
        assertTrue(adapter.infos.stream().noneMatch(l -> l.contains("SECRET-JWT")));
    }

    @Test
    void nameConflictGetsItsOwnBanner() throws Exception {
        startConnector("conflict");
        await("conflict", () -> adapter.warnings.stream().anyMatch(l -> l.contains("already has a game server with this name")));
        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("(HTTP 409)")));
    }
}
