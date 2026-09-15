package io.takaro.zomboid.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.net.URI;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

/** The app-level {@code ping} -> {@code pong} heartbeat (core fix a). */
class PingPongTest {

    private TakaroWebSocketClient client;
    private List<String> sentMessages;
    private List<String> warnings;

    @BeforeEach
    void setUp() throws Exception {
        sentMessages = new ArrayList<>();
        warnings = new ArrayList<>();
        GameAdapter adapter = new MinimalAdapter(warnings);
        TakaroConfig config = new TakaroConfig();
        client = new TakaroWebSocketClient(new URI("ws://localhost:9999"), adapter, config) {
            @Override public void send(String text) { sentMessages.add(text); }
            @Override public boolean isOpen() { return true; }
        };
    }

    @Test
    void pingProducesPong() {
        client.onMessage("{\"type\":\"ping\"}");
        assertEquals(1, sentMessages.size());
        JsonObject reply = JsonParser.parseString(sentMessages.get(0)).getAsJsonObject();
        assertEquals("pong", reply.get("type").getAsString());
        // A ping must NOT fall through to the "unknown message type" default.
        assertTrue(warnings.stream().noneMatch(w -> w.contains("Unknown message type")));
    }

    @Test
    void pingEchoesRequestId() {
        client.onMessage("{\"type\":\"ping\",\"requestId\":\"hb-1\"}");
        JsonObject reply = JsonParser.parseString(sentMessages.get(0)).getAsJsonObject();
        assertEquals("pong", reply.get("type").getAsString());
        assertEquals("hb-1", reply.get("requestId").getAsString());
    }

    @Test
    void unknownTypeStillWarns() {
        client.onMessage("{\"type\":\"bogus\"}");
        assertTrue(warnings.stream().anyMatch(w -> w.contains("Unknown message type")));
    }

    private static class MinimalAdapter implements GameAdapter {
        private final List<String> warnings;
        MinimalAdapter(List<String> warnings) { this.warnings = warnings; }
        @Override public void logInfo(String msg) {}
        @Override public void logWarning(String msg) { warnings.add(msg); }
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
