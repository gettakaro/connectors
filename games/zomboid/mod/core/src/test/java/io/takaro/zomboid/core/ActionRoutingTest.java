package io.takaro.zomboid.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
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
        adapter.knownPlayer = new PlayerInfo("abc-123", "Steve", null, null, null, "steam:abc-123", "127.0.0.1", 42);
        sendRequest("getPlayer", "req-3", "{\"gameId\":\"abc-123\"}");
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        JsonObject payload = response.getAsJsonObject("payload");
        assertEquals("abc-123", payload.get("gameId").getAsString());
        assertEquals("Steve", payload.get("name").getAsString());
        assertEquals("steam:abc-123", payload.get("platformId").getAsString());
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

        assertEquals("Server: hello world", adapter.lastMessage);
        assertNull(adapter.lastRecipient);
    }

    @Test
    void sendMessageWithRecipient() {
        sendRequest("sendMessage", "req-7", "{\"message\":\"hi\",\"opts\":{\"recipient\":{\"gameId\":\"player-1\"}}}");
        waitForResponse();

        assertEquals("Server: hi", adapter.lastMessage);
        assertEquals("player-1", adapter.lastRecipient);
    }

    @Test
    void sendMessageUsesSenderNameOverride() {
        sendRequest("sendMessage", "req-7b", "{\"message\":\"yo\",\"opts\":{\"senderNameOverride\":\"Admin\"}}");
        waitForResponse();

        assertEquals("Admin: yo", adapter.lastMessage);
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

    // --- explicit JSON null in optional args (defect 2026-09-16, LANE-C cell C8) ---
    // Takaro *modules* send optional args as explicit nulls where the REST path
    // omits the key; Gson's has() is true for those and JsonNull.getAsString()
    // throws, which silently dropped module-driven teleports.

    @Test
    void teleportPlayerWithNullDimensionSucceeds() {
        sendRequest("teleportPlayer", "req-null-dim",
                "{\"player\":{\"gameId\":\"player-1\"},\"x\":10911,\"y\":10037,\"z\":0,\"dimension\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"), "teleport with dimension:null must not error");
        assertEquals("player-1", adapter.lastTeleportedId);
        assertEquals(10911, adapter.lastTeleportX, 0.001);
        assertEquals(10037, adapter.lastTeleportY, 0.001);
        assertEquals(0, adapter.lastTeleportZ, 0.001);
        assertNull(adapter.lastTeleportDimension);
    }

    @Test
    void kickPlayerWithNullReasonSucceeds() {
        sendRequest("kickPlayer", "req-null-reason",
                "{\"player\":{\"gameId\":\"player-1\"},\"reason\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("player-1", adapter.lastKickedId);
        assertEquals("", adapter.lastKickReason);
    }

    @Test
    void banPlayerWithNullExpiresAtIsPermanent() {
        sendRequest("banPlayer", "req-null-expiry",
                "{\"player\":{\"gameId\":\"player-1\"},\"reason\":null,\"expiresAt\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("player-1", adapter.lastBannedId);
        assertEquals("", adapter.lastBanReason);
        assertNull(adapter.lastBanExpiry, "expiresAt:null must mean a permanent ban");
    }

    @Test
    void banPlayerWithIsoExpiresAtIsPassedThrough() {
        sendRequest("banPlayer", "req-iso-expiry",
                "{\"player\":{\"gameId\":\"player-1\"},\"reason\":\"cheating\",\"expiresAt\":\"2026-12-31T00:00:00Z\"}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("cheating", adapter.lastBanReason);
        assertEquals("2026-12-31T00:00:00Z", adapter.lastBanExpiry);
    }

    @Test
    void giveItemWithNullQualityDeliversItem() {
        sendRequest("giveItem", "req-null-quality",
                "{\"player\":{\"gameId\":\"player-1\"},\"item\":\"Base.Axe\",\"amount\":1,\"quality\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("Base.Axe", adapter.lastGivenItem);
        assertEquals(1, adapter.lastGivenAmount);
        assertEquals("", adapter.lastGivenQuality);
    }

    @Test
    void sendMessageWithNullOptsAndRecipientSucceeds() {
        sendRequest("sendMessage", "req-null-opts",
                "{\"message\":\"hi\",\"opts\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("Server: hi", adapter.lastMessage);
        assertNull(adapter.lastRecipient);
    }

    @Test
    void executeConsoleCommandWithNullCommandDoesNotThrow() {
        sendRequest("executeConsoleCommand", "req-null-cmd", "{\"command\":null}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        assertFalse(response.has("error"));
        assertEquals("", adapter.lastCommand);
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
        adapter.items.add(new GameItem("steam:stone", "stone", "A block"));
        sendRequest("listItems", "req-11", "{}");
        waitForResponse();

        JsonObject response = parseResponse(sentMessages.get(0));
        JsonArray items = response.getAsJsonArray("payload");
        assertEquals(1, items.size());
        assertEquals("steam:stone", items.get(0).getAsJsonObject().get("code").getAsString());
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
    void allSeventeenActionsReturnWellFormedEnvelope() {
        // Minimal valid args per action; the RecordingAdapter returns empty
        // data for everything, so we only assert envelope shape here.
        String[][] actions = {
                {"testReachability", "{}"},
                {"getPlayers", "{}"},
                {"getPlayer", "{\"gameId\":\"p1\"}"},
                {"getPlayerLocation", "{\"gameId\":\"p1\"}"},
                {"getPlayerInventory", "{\"gameId\":\"p1\"}"},
                {"giveItem", "{\"player\":{\"gameId\":\"p1\"},\"item\":\"Base.Axe\",\"amount\":1}"},
                {"listItems", "{}"},
                {"listEntities", "{}"},
                {"listLocations", "{}"},
                {"executeConsoleCommand", "{\"command\":\"players\"}"},
                {"sendMessage", "{\"message\":\"hi\"}"},
                {"teleportPlayer", "{\"player\":{\"gameId\":\"p1\"},\"x\":1,\"y\":2,\"z\":3}"},
                {"kickPlayer", "{\"player\":{\"gameId\":\"p1\"},\"reason\":\"x\"}"},
                {"banPlayer", "{\"player\":{\"gameId\":\"p1\"},\"reason\":\"x\"}"},
                {"unbanPlayer", "{\"gameId\":\"p1\"}"},
                {"listBans", "{}"},
                {"shutdown", "{}"},
        };
        assertEquals(17, actions.length, "expected 17 M1/M2 actions");
        for (String[] a : actions) {
            sentMessages.clear();
            sendRequest(a[0], "req-" + a[0], a[1]);
            waitForResponse();
            assertEquals(1, sentMessages.size(), "action " + a[0] + " must reply exactly once");
            JsonObject response = parseResponse(sentMessages.get(0));
            assertEquals("response", response.get("type").getAsString(), a[0]);
            assertEquals("req-" + a[0], response.get("requestId").getAsString(), a[0]);
            // Well-formed envelope: either a payload or an error, never both missing.
            assertTrue(response.has("payload") || response.has("error"),
                    "action " + a[0] + " envelope missing payload/error");
        }
    }

    @Test
    void adapterExceptionBecomesErrorResponse() {
        // UnsupportedOperationException from a not-yet-implemented action must
        // become a clean protocol error, not hang and not crash.
        adapter.throwOnCommand = true;
        sendRequest("executeConsoleCommand", "req-throw", "{\"command\":\"boom\"}");
        waitForResponse();
        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertTrue(response.has("error"));
    }

    @Test
    void argsAsEmptyArrayIsTolerated() {
        JsonObject request = new JsonObject();
        request.addProperty("type", "request");
        request.addProperty("requestId", "req-arr");
        JsonObject payload = new JsonObject();
        payload.addProperty("action", "getPlayers");
        payload.add("args", JsonParser.parseString("[]"));
        request.add("payload", payload);

        client.onMessage(request.toString());
        waitForResponse();

        assertEquals(1, sentMessages.size());
        JsonObject response = parseResponse(sentMessages.get(0));
        assertTrue(response.get("payload").isJsonArray());
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
        client.emitPlayerConnected(new PlayerInfo("uuid-1", "Steve", null, null, null, "steam:uuid-1", "1.2.3.4", 10));

        assertEquals(1, sentMessages.size());
        JsonObject msg = parseResponse(sentMessages.get(0));
        assertEquals("gameEvent", msg.get("type").getAsString());
        JsonObject eventPayload = msg.getAsJsonObject("payload");
        assertEquals("player-connected", eventPayload.get("type").getAsString());
        assertEquals("uuid-1", eventPayload.getAsJsonObject("data").getAsJsonObject("player").get("gameId").getAsString());
        assertEquals("steam:uuid-1", eventPayload.getAsJsonObject("data").getAsJsonObject("player").get("platformId").getAsString());
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
        String lastTeleportedId = null;
        double lastTeleportX;
        double lastTeleportY;
        double lastTeleportZ;
        String lastTeleportDimension = null;
        String lastGivenItem = null;
        int lastGivenAmount;
        String lastGivenQuality = null;
        List<BanEntry> bans = new ArrayList<>();
        List<GameItem> items = new ArrayList<>();
        boolean throwOnCommand = false;

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
            lastGivenItem = itemCode;
            lastGivenAmount = amount;
            lastGivenQuality = quality;
        }

        @Override
        public void sendMessage(String message, String recipientGameId) {
            lastMessage = message;
            lastRecipient = recipientGameId;
        }

        @Override
        public CommandResult executeConsoleCommand(String command) {
            if (throwOnCommand) {
                throw new UnsupportedOperationException("not implemented in M1");
            }
            lastCommand = command;
            return new CommandResult(true, "", null);
        }

        @Override
        public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {
            lastTeleportedId = gameId;
            lastTeleportX = x;
            lastTeleportY = y;
            lastTeleportZ = z;
            lastTeleportDimension = dimension;
        }

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
