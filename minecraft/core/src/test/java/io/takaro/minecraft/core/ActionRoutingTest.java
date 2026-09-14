package io.takaro.minecraft.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.model.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.net.URI;
import java.util.*;

import static org.junit.jupiter.api.Assertions.*;

class ActionRoutingTest {

    private RecordingAdapter adapter;
    private TakaroWebSocketClient client;
    private List<String> sentMessages;

    @BeforeEach
    void setUp() throws Exception {
        adapter = new RecordingAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setDebugEnabled(false);

        // Create client with a dummy URI - we won't actually connect
        client = new TakaroWebSocketClient(new URI("ws://localhost:9999"), adapter, config) {
            @Override
            public void send(String text) {
                sentMessages.add(text);
            }

            @Override
            public boolean isOpen() {
                return true;
            }
        };
        sentMessages = new ArrayList<>();
    }

    @Test
    void testReachabilityReturnsConnectable() {
        sendRequest("testReachability", "req-1", "{}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertEquals("response", response.get("type").getAsString());
        assertEquals("req-1", response.get("requestId").getAsString());
        assertTrue(response.getAsJsonObject("payload").get("connectable").getAsBoolean());
    }

    @Test
    void getPlayersReturnsEmptyArray() {
        sendRequest("getPlayers", "req-2", "{}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertTrue(response.get("payload").isJsonArray());
        assertEquals(0, response.getAsJsonArray("payload").size());
    }

    @Test
    void getPlayerWithKnownIdReturnsPlayer() {
        adapter.knownPlayer = new PlayerInfo("abc-123", "Steve", null, null, null, "minecraft:abc-123", "127.0.0.1", 42);
        sendRequest("getPlayer", "req-3", "{\"gameId\":\"abc-123\"}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        JsonObject payload = response.getAsJsonObject("payload");
        assertEquals("abc-123", payload.get("gameId").getAsString());
        assertEquals("Steve", payload.get("name").getAsString());
        assertEquals("minecraft:abc-123", payload.get("platformId").getAsString());
        assertEquals(42, payload.get("ping").getAsInt());
    }

    @Test
    void getPlayerWithUnknownIdReturnsEmptyPayload() {
        sendRequest("getPlayer", "req-4", "{\"gameId\":\"unknown\"}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        // Should return empty object, not null
        assertTrue(response.has("payload"));
    }

    @Test
    void executeConsoleCommandReturnsResult() {
        sendRequest("executeConsoleCommand", "req-5", "{\"command\":\"say hello\"}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        JsonObject payload = response.getAsJsonObject("payload");
        assertTrue(payload.get("success").getAsBoolean());
        assertEquals("say hello", adapter.lastCommand);
    }

    @Test
    void sendMessageCallsAdapter() {
        sendRequest("sendMessage", "req-6", "{\"message\":\"hello world\"}");
        waitForResponse();

        assertEquals("hello world", adapter.lastMessage);
        assertNull(adapter.lastRecipient);
    }

    @Test
    void sendMessageWithRecipient() {
        sendRequest("sendMessage", "req-7", "{\"message\":\"hi\",\"opts\":{\"recipient\":{\"gameId\":\"player-1\"}}}");
        waitForResponse();

        assertEquals("hi", adapter.lastMessage);
        assertEquals("player-1", adapter.lastRecipient);
    }

    @Test
    void kickPlayerCallsAdapter() {
        sendRequest("kickPlayer", "req-8", "{\"player\":{\"gameId\":\"player-1\"},\"reason\":\"bad behavior\"}");
        waitForResponse();

        assertEquals("player-1", adapter.lastKickedId);
        assertEquals("bad behavior", adapter.lastKickReason);
    }

    @Test
    void banPlayerCallsAdapter() {
        sendRequest("banPlayer", "req-9", "{\"player\":{\"gameId\":\"player-1\"},\"reason\":\"cheating\",\"expiresAt\":\"2026-12-31T00:00:00Z\"}");
        waitForResponse();

        assertEquals("player-1", adapter.lastBannedId);
        assertEquals("cheating", adapter.lastBanReason);
        assertEquals("2026-12-31T00:00:00Z", adapter.lastBanExpiry);
    }

    @Test
    void listBansReturnsFormattedEntries() {
        adapter.bans.add(new BanEntry("uuid-1", "Griefer", "griefing", "2026-12-31T00:00:00Z"));
        sendRequest("listBans", "req-10", "{}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        JsonArray bans = response.getAsJsonArray("payload");
        assertEquals(1, bans.size());
        JsonObject ban = bans.get(0).getAsJsonObject();
        assertEquals("uuid-1", ban.getAsJsonObject("player").get("gameId").getAsString());
        assertEquals("Griefer", ban.getAsJsonObject("player").get("name").getAsString());
        assertEquals("griefing", ban.get("reason").getAsString());
    }

    @Test
    void listItemsReturnsArray() {
        adapter.items.add(new GameItem("minecraft:stone", "stone", "A block"));
        sendRequest("listItems", "req-11", "{}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        JsonArray items = response.getAsJsonArray("payload");
        assertEquals(1, items.size());
        assertEquals("minecraft:stone", items.get(0).getAsJsonObject().get("code").getAsString());
    }

    @Test
    void unknownActionReturnsError() {
        sendRequest("nonExistentAction", "req-err", "{}");
        // Unknown actions respond synchronously, no need to wait
        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertTrue(response.has("error"));
        assertTrue(response.get("error").getAsString().contains("Action not implemented"));
    }

    @Test
    void argsAsJsonStringIsParsed() {
        // args is a JSON string (the normal protocol format)
        sendRequest("executeConsoleCommand", "req-str", "\"{ \\\"command\\\": \\\"help\\\" }\"");
        // Actually, the args field contains a JSON string. Let me construct it properly.
        sentMessages.clear();

        JsonObject request = new JsonObject();
        request.addProperty("type", "request");
        request.addProperty("requestId", "req-str");
        JsonObject payload = new JsonObject();
        payload.addProperty("action", "executeConsoleCommand");
        payload.addProperty("args", "{\"command\":\"help\"}");
        request.add("payload", payload);

        client.onMessage(request.toString());
        waitForResponse();

        assertEquals("help", adapter.lastCommand);
    }

    @Test
    void argsAsJsonObjectIsParsed() {
        JsonObject request = new JsonObject();
        request.addProperty("type", "request");
        request.addProperty("requestId", "req-obj");
        JsonObject payload = new JsonObject();
        payload.addProperty("action", "executeConsoleCommand");
        JsonObject args = new JsonObject();
        args.addProperty("command", "list");
        payload.add("args", args);
        request.add("payload", payload);

        client.onMessage(request.toString());
        waitForResponse();

        assertEquals("list", adapter.lastCommand);
    }

    @Test
    void eventEmitterPlayerConnected() {
        client.emitPlayerConnected(new PlayerInfo("uuid-1", "Steve", null, null, null, "minecraft:uuid-1", "1.2.3.4", 10));

        assertEquals(1, sentMessages.size());
        JsonObject msg = parseResponse(sentMessages.get(0));
        assertEquals("gameEvent", msg.get("type").getAsString());
        JsonObject eventPayload = msg.getAsJsonObject("payload");
        assertEquals("player-connected", eventPayload.get("type").getAsString());
        assertEquals("uuid-1", eventPayload.getAsJsonObject("data").getAsJsonObject("player").get("gameId").getAsString());
        assertEquals("minecraft:uuid-1", eventPayload.getAsJsonObject("data").getAsJsonObject("player").get("platformId").getAsString());
    }

    @Test
    void eventEmitterChatMessage() {
        client.emitChatMessage("uuid-1", "Steve", "global", "hello everyone");

        assertEquals(1, sentMessages.size());
        JsonObject msg = parseResponse(sentMessages.get(0));
        assertEquals("gameEvent", msg.get("type").getAsString());
        JsonObject data = msg.getAsJsonObject("payload").getAsJsonObject("data");
        assertEquals("global", data.get("channel").getAsString());
        assertEquals("hello everyone", data.get("msg").getAsString());
    }

    @Test
    void eventEmitterPlayerDeath() {
        client.emitPlayerDeath("victim-id", "Victim", "killer-id", "Killer", 10.0, 64.0, 20.0, "overworld");

        assertEquals(1, sentMessages.size());
        JsonObject msg = parseResponse(sentMessages.get(0));
        JsonObject data = msg.getAsJsonObject("payload").getAsJsonObject("data");
        assertEquals("victim-id", data.getAsJsonObject("player").get("gameId").getAsString());
        assertEquals("killer-id", data.getAsJsonObject("attacker").get("gameId").getAsString());
        assertEquals(64.0, data.getAsJsonObject("position").get("y").getAsDouble());
    }

    @Test
    void eventEmitterPlayerDeathNoAttacker() {
        client.emitPlayerDeath("victim-id", "Victim", null, null, 0, 0, 0, "nether");

        JsonObject data = parseResponse(sentMessages.get(0)).getAsJsonObject("payload").getAsJsonObject("data");
        assertFalse(data.has("attacker"));
    }

    @Test
    void giveItemWithNullQualityUsesEmptyString() {
        // Takaro shop deliveries send "quality": null - must not throw (JsonNull)
        sendRequest("giveItem", "req-give-null", "{\"player\":{\"gameId\":\"player-1\"},\"item\":\"minecraft:diamond\",\"amount\":1,\"quality\":null}");
        waitForResponse();

        assertEquals("player-1", adapter.lastGiveGameId);
        assertEquals("minecraft:diamond", adapter.lastGiveItem);
        assertEquals(1, adapter.lastGiveAmount);
        assertEquals("", adapter.lastGiveQuality);

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"), "giveItem with null quality must not produce an error response");
        assertEquals("req-give-null", response.get("requestId").getAsString());
    }

    @Test
    void giveItemWithMissingQualityUsesEmptyString() {
        sendRequest("giveItem", "req-give-missing", "{\"player\":{\"gameId\":\"player-2\"},\"item\":\"minecraft:stone\",\"amount\":5}");
        waitForResponse();

        assertEquals("player-2", adapter.lastGiveGameId);
        assertEquals(5, adapter.lastGiveAmount);
        assertEquals("", adapter.lastGiveQuality);
        assertFalse(parseResponse(sentMessages.get(0)).has("error"));
    }

    @Test
    void giveItemWithQualityPassesItThrough() {
        sendRequest("giveItem", "req-give-q", "{\"player\":{\"gameId\":\"player-3\"},\"item\":\"minecraft:stone\",\"quality\":\"5\"}");
        waitForResponse();

        assertEquals("5", adapter.lastGiveQuality);
        assertEquals(1, adapter.lastGiveAmount);
    }

    @Test
    void nullOptionalStringsDoNotThrowForOtherActions() {
        sendRequest("banPlayer", "req-ban-null", "{\"player\":{\"gameId\":\"player-1\"},\"reason\":null,\"expiresAt\":null}");
        waitForResponse();
        assertEquals("player-1", adapter.lastBannedId);
        assertEquals("", adapter.lastBanReason);
        assertNull(adapter.lastBanExpiry);
        assertFalse(parseResponse(sentMessages.get(0)).has("error"));

        sentMessages.clear();
        sendRequest("kickPlayer", "req-kick-null", "{\"player\":{\"gameId\":\"player-1\"},\"reason\":null}");
        waitForResponse();
        assertEquals("", adapter.lastKickReason);
        assertFalse(parseResponse(sentMessages.get(0)).has("error"));

        sentMessages.clear();
        sendRequest("sendMessage", "req-msg-null", "{\"message\":\"hi\",\"opts\":null}");
        waitForResponse();
        assertEquals("hi", adapter.lastMessage);
        assertNull(adapter.lastRecipient);
        assertFalse(parseResponse(sentMessages.get(0)).has("error"));

        sentMessages.clear();
        sendRequest("teleportPlayer", "req-tp-null", "{\"player\":{\"gameId\":\"player-1\"},\"x\":1,\"y\":2,\"z\":3,\"dimension\":null}");
        waitForResponse();
        assertFalse(parseResponse(sentMessages.get(0)).has("error"));
    }

    // --- Helpers ---

    private void sendRequest(String action, String requestId, String argsJson) {
        JsonObject request = new JsonObject();
        request.addProperty("type", "request");
        request.addProperty("requestId", requestId);
        JsonObject payload = new JsonObject();
        payload.addProperty("action", action);
        payload.addProperty("args", argsJson);
        request.add("payload", payload);

        client.onMessage(request.toString());
    }

    private void waitForResponse() {
        // runOnMainThread executes synchronously in tests, but whenCompleteAsync
        // runs on ForkJoinPool. Give it a moment.
        try { Thread.sleep(100); } catch (InterruptedException ignored) {}
    }

    private JsonObject parseResponse(String json) {
        return JsonParser.parseString(json).getAsJsonObject();
    }

    // --- Test adapter that records calls ---

    private static class RecordingAdapter implements GameAdapter {
        PlayerInfo knownPlayer = null;
        String lastCommand = null;
        String lastMessage = null;
        String lastRecipient = null;
        String lastKickedId = null;
        String lastKickReason = null;
        String lastBannedId = null;
        String lastBanReason = null;
        String lastBanExpiry = null;
        String lastGiveGameId = null;
        String lastGiveItem = null;
        int lastGiveAmount = -1;
        String lastGiveQuality = null;
        List<BanEntry> bans = new ArrayList<>();
        List<GameItem> items = new ArrayList<>();

        @Override public void logInfo(String msg) {}
        @Override public void logWarning(String msg) {}
        @Override public void logDebug(String msg) {}
        @Override public void runOnMainThread(Runnable task) { task.run(); }
        @Override public void setEventEmitter(EventEmitter emitter) {}

        @Override
        public PlayerInfo getPlayer(String gameId) {
            return knownPlayer != null && knownPlayer.gameId().equals(gameId) ? knownPlayer : null;
        }

        @Override public List<PlayerInfo> getPlayers() { return Collections.emptyList(); }
        @Override public PlayerLocation getPlayerLocation(String gameId) { return null; }
        @Override public List<InventoryItem> getPlayerInventory(String gameId) { return Collections.emptyList(); }
        @Override public List<GameItem> listItems() { return items; }
        @Override public List<GameEntity> listEntities() { return Collections.emptyList(); }
        @Override public List<GameLocation> listLocations() { return Collections.emptyList(); }
        @Override
        public void giveItem(String gameId, String itemCode, int amount, String quality) {
            lastGiveGameId = gameId;
            lastGiveItem = itemCode;
            lastGiveAmount = amount;
            lastGiveQuality = quality;
        }

        @Override
        public void sendMessage(String message, String recipientGameId) {
            lastMessage = message;
            lastRecipient = recipientGameId;
        }

        @Override
        public CommandResult executeConsoleCommand(String command) {
            lastCommand = command;
            return new CommandResult(true, "", null);
        }

        @Override public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {}

        @Override
        public void kickPlayer(String gameId, String reason) {
            lastKickedId = gameId;
            lastKickReason = reason;
        }

        @Override
        public void banPlayer(String gameId, String reason, String expiresAt) {
            lastBannedId = gameId;
            lastBanReason = reason;
            lastBanExpiry = expiresAt;
        }

        @Override public void unbanPlayer(String gameId) {}
        @Override public List<BanEntry> listBans() { return bans; }
        @Override public void shutdownServer() {}
    }
}
