package io.takaro.zomboid.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroWebSocketClient extends WebSocketClient implements EventEmitter {

    private final GameAdapter adapter;
    private final TakaroConfig config;
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "takaro-reconnect");
        t.setDaemon(true);
        return t;
    });
    private volatile long currentReconnectDelay;
    private volatile boolean shouldReconnect = true;

    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config) {
        super(serverUri);
        this.adapter = adapter;
        this.config = config;
        this.currentReconnectDelay = config.getReconnectDelay();
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        adapter.logInfo("WebSocket connected, sending identify...");
        sendIdentify();
        try {
            adapter.onConnectionEstablished();
        } catch (Exception e) {
            adapter.logWarning("onConnectionEstablished failed: " + e.getMessage());
        }
    }

    @Override
    public void onMessage(String message) {
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS RECV: " + message);
        }
        try {
            JsonObject json = JsonParser.parseString(message).getAsJsonObject();
            String type = optString(json, "type", "");

            switch (type) {
                case "connected":
                    adapter.logInfo("Received server hello");
                    break;
                case "identifyResponse":
                    handleIdentifyResponse(json);
                    break;
                case "authenticated":
                    adapter.logInfo("Authentication confirmed");
                    currentReconnectDelay = config.getReconnectDelay();
                    break;
                case "request":
                    handleRequest(json);
                    break;
                case "ping":
                    handlePing(json);
                    break;
                case "error":
                    JsonObject errorPayload = optObject(json, "payload");
                    String errorMsg = optString(errorPayload, "message", null);
                    if (errorMsg == null) errorMsg = optString(json, "message", "unknown");
                    String requestId = optString(json, "requestId", null);
                    adapter.logWarning("Server error: " + errorMsg + (requestId != null ? " (requestId=" + requestId + ")" : ""));
                    break;
                default:
                    adapter.logWarning("Unknown message type: " + type);
                    break;
            }
        } catch (Exception e) {
            adapter.logWarning("Failed to parse message: " + e.getMessage());
        }
    }

    @Override
    public void onClose(int code, String reason, boolean remote) {
        adapter.logInfo("WebSocket closed (code=" + code + ", reason=" + reason + ", remote=" + remote + ")");

        if (code == 1008 || code == 4001 || code == 4003) {
            adapter.logWarning("Authentication error, disabling reconnect");
            shouldReconnect = false;
        }

        if (shouldReconnect && config.isReconnectEnabled()) {
            scheduleReconnect();
        }
    }

    @Override
    public void onError(Exception ex) {
        adapter.logWarning("WebSocket error: " + ex.getMessage());
    }

    public void shutdown() {
        shouldReconnect = false;
        scheduler.shutdownNow();
        try {
            closeBlocking();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    // --- EventEmitter implementation ---

    @Override
    public void emitPlayerConnected(PlayerInfo player) {
        JsonObject data = new JsonObject();
        data.add("player", playerInfoToJson(player));
        sendGameEvent("player-connected", data);
    }

    @Override
    public void emitPlayerDisconnected(PlayerInfo player) {
        JsonObject data = new JsonObject();
        data.add("player", playerInfoToJson(player));
        sendGameEvent("player-disconnected", data);
    }

    @Override
    public void emitChatMessage(String gameId, String playerName, String channel, String message) {
        JsonObject data = new JsonObject();
        JsonObject player = new JsonObject();
        player.addProperty("gameId", gameId);
        player.addProperty("name", playerName);
        data.add("player", player);
        data.addProperty("channel", channel);
        data.addProperty("msg", message);
        sendGameEvent("chat-message", data);
    }

    @Override
    public void emitPlayerDeath(String gameId, String playerName, String attackerGameId, String attackerName, double x, double y, double z, String dimension) {
        JsonObject data = new JsonObject();
        JsonObject player = new JsonObject();
        player.addProperty("gameId", gameId);
        player.addProperty("name", playerName);
        data.add("player", player);

        if (attackerGameId != null) {
            JsonObject attacker = new JsonObject();
            attacker.addProperty("gameId", attackerGameId);
            attacker.addProperty("name", attackerName);
            data.add("attacker", attacker);
        }

        JsonObject position = new JsonObject();
        position.addProperty("x", x);
        position.addProperty("y", y);
        position.addProperty("z", z);
        if (dimension != null) {
            position.addProperty("dimension", dimension);
        }
        data.add("position", position);
        sendGameEvent("player-death", data);
    }

    @Override
    public void emitEntityKilled(String gameId, String playerName, String entityCode, String weaponCode) {
        JsonObject data = new JsonObject();
        JsonObject player = new JsonObject();
        player.addProperty("gameId", gameId);
        player.addProperty("name", playerName);
        data.add("player", player);
        data.addProperty("entity", entityCode);
        data.addProperty("weapon", weaponCode != null ? weaponCode : "");
        sendGameEvent("entity-killed", data);
    }

    @Override
    public void emitLog(String message) {
        JsonObject data = new JsonObject();
        data.addProperty("msg", message);
        sendGameEvent("log", data);
    }

    // --- Private helpers ---

    private void sendGameEvent(String eventType, JsonObject data) {
        if (!isOpen()) return;

        JsonObject payload = new JsonObject();
        payload.addProperty("type", eventType);
        payload.add("data", data);

        JsonObject msg = new JsonObject();
        msg.addProperty("type", "gameEvent");
        msg.add("payload", payload);

        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND gameEvent: " + msg);
        }
        send(msg.toString());
    }

    private void sendIdentify() {
        JsonObject payload = new JsonObject();
        String identity = config.getIdentityToken();
        String registration = config.getRegistrationToken();
        payload.addProperty("identityToken", identity != null ? identity : "");
        payload.addProperty("registrationToken", registration != null ? registration : "");

        JsonObject msg = new JsonObject();
        msg.addProperty("type", "identify");
        msg.add("payload", payload);
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND identify (tokens redacted)");
        }
        send(msg.toString());
    }

    private void handleIdentifyResponse(JsonObject json) {
        JsonObject payloadObj = optObject(json, "payload");
        JsonObject payload = payloadObj != null ? payloadObj : new JsonObject();

        if (payload.has("error") && !payload.get("error").isJsonNull()) {
            JsonElement errorElement = payload.get("error");
            String errorMessage;
            if (errorElement.isJsonObject()) {
                JsonObject errorObj = errorElement.getAsJsonObject();
                errorMessage = optString(errorObj, "message", errorObj.toString());
            } else {
                errorMessage = errorElement.getAsString();
            }
            adapter.logWarning("Identify failed: " + errorMessage);
            return;
        }

        currentReconnectDelay = config.getReconnectDelay();

        String serverId = optString(optObject(payload, "server"), "id", null);
        if (serverId != null) {
            adapter.logInfo("Identified successfully, server ID: " + serverId);
        } else {
            adapter.logInfo("Identified successfully");
        }
    }

    /**
     * Application-level heartbeat. Takaro sends {@code {"type":"ping"}} and
     * expects a {@code {"type":"pong"}} back; without this handler a ping falls
     * through to the "unknown message type" default and is dropped.
     */
    private void handlePing(JsonObject json) {
        if (!isOpen()) return;
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "pong");
        if (json.has("requestId")) {
            msg.add("requestId", json.get("requestId"));
        }
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND pong");
        }
        send(msg.toString());
    }

    private void handleRequest(JsonObject json) {
        String requestId = optString(json, "requestId", null);
        if (requestId == null) {
            adapter.logWarning("Received request without requestId");
            return;
        }

        JsonObject requestPayload = optObject(json, "payload");
        JsonObject payload = requestPayload != null ? requestPayload : new JsonObject();
        String action = optString(payload, "action", "");
        if (config.isDebugEnabled()) {
            adapter.logDebug("Request: action=" + action + ", requestId=" + requestId);
        }

        JsonObject args = parseArgs(payload.has("args") ? payload.get("args") : null);

        switch (action) {
            case "testReachability":
                handleTestReachability(requestId);
                break;
            case "getPlayer":
                handleOnMainThread(requestId, () -> handleGetPlayer(args));
                break;
            case "getPlayers":
                handleOnMainThread(requestId, () -> handleGetPlayers());
                break;
            case "getPlayerLocation":
                handleOnMainThread(requestId, () -> handleGetPlayerLocation(args));
                break;
            case "getPlayerInventory":
                handleOnMainThread(requestId, () -> handleGetPlayerInventory(args));
                break;
            case "giveItem":
                handleOnMainThread(requestId, () -> { handleGiveItem(args); return null; });
                break;
            case "listItems":
                handleOnMainThread(requestId, () -> handleListItems());
                break;
            case "listEntities":
                handleOnMainThread(requestId, () -> handleListEntities());
                break;
            case "listLocations":
                handleOnMainThread(requestId, () -> handleListLocations());
                break;
            case "executeConsoleCommand":
                handleOnMainThread(requestId, () -> handleExecuteConsoleCommand(args));
                break;
            case "sendMessage":
                handleOnMainThread(requestId, () -> { handleSendMessage(args); return null; });
                break;
            case "teleportPlayer":
                handleOnMainThread(requestId, () -> { handleTeleportPlayer(args); return null; });
                break;
            case "kickPlayer":
                handleOnMainThread(requestId, () -> { handleKickPlayer(args); return null; });
                break;
            case "banPlayer":
                handleOnMainThread(requestId, () -> { handleBanPlayer(args); return null; });
                break;
            case "unbanPlayer":
                handleOnMainThread(requestId, () -> { handleUnbanPlayer(args); return null; });
                break;
            case "listBans":
                handleOnMainThread(requestId, () -> handleListBans());
                break;
            case "shutdown":
                handleOnMainThread(requestId, () -> { adapter.shutdownServer(); return null; });
                break;
            default:
                sendResponse(requestId, null, "Action not implemented: " + action);
                break;
        }
    }

    /**
     * Optional string field. Returns {@code def} when the key is absent, is an
     * explicit JSON {@code null} or is not a primitive. Takaro modules send
     * optional args as explicit nulls (e.g. {@code "dimension": null}) where the
     * REST path omits the key entirely, and {@code JsonNull.getAsString()}
     * throws, so every optional read must go through here.
     */
    static String optString(JsonObject obj, String key, String def) {
        if (obj == null || !obj.has(key)) return def;
        JsonElement el = obj.get(key);
        return el.isJsonPrimitive() ? el.getAsString() : def;
    }

    /** Optional number field; {@code def} when absent, JSON null or unparsable. */
    static double optDouble(JsonObject obj, String key, double def) {
        if (obj == null || !obj.has(key)) return def;
        JsonElement el = obj.get(key);
        if (!el.isJsonPrimitive()) return def;
        try {
            return el.getAsDouble();
        } catch (RuntimeException e) {
            return def;
        }
    }

    /** Optional integer field; {@code def} when absent, JSON null or unparsable. */
    static int optInt(JsonObject obj, String key, int def) {
        if (obj == null || !obj.has(key)) return def;
        JsonElement el = obj.get(key);
        if (!el.isJsonPrimitive()) return def;
        try {
            return el.getAsInt();
        } catch (RuntimeException e) {
            return def;
        }
    }

    /** Optional object field; {@code null} when absent, JSON null or not an object. */
    static JsonObject optObject(JsonObject obj, String key) {
        if (obj == null || !obj.has(key)) return null;
        JsonElement el = obj.get(key);
        return el.isJsonObject() ? el.getAsJsonObject() : null;
    }

    /**
     * Normalise the {@code args} field into a {@link JsonObject}. Takaro sends
     * args most often as a JSON string, sometimes as a raw object, and empty
     * args as {@code "{}"}, {@code "[]"}, {@code {}}, {@code []} or an empty
     * string. Anything that is not an object becomes an empty object so the
     * action handlers never NPE on a missing/odd shape.
     */
    static JsonObject parseArgs(JsonElement argsElement) {
        if (argsElement == null || argsElement.isJsonNull()) {
            return new JsonObject();
        }
        JsonElement parsed = argsElement;
        if (argsElement.isJsonPrimitive() && argsElement.getAsJsonPrimitive().isString()) {
            String argsStr = argsElement.getAsString().trim();
            if (argsStr.isEmpty()) {
                return new JsonObject();
            }
            try {
                parsed = JsonParser.parseString(argsStr);
            } catch (RuntimeException e) {
                return new JsonObject();
            }
        }
        return parsed.isJsonObject() ? parsed.getAsJsonObject() : new JsonObject();
    }

    /**
     * Resolve a player's gameId from the many arg shapes Takaro uses: a flat
     * {@code {gameId}}, a nested {@code {player:{gameId}}} or
     * {@code {playerRef:{gameId}}}.
     */
    static String extractGameId(JsonObject args) {
        if (args == null) return null;
        String flat = optString(args, "gameId", null);
        if (flat != null) return flat;
        for (String key : new String[] {"player", "playerRef"}) {
            String nested = optString(optObject(args, key), "gameId", null);
            if (nested != null) return nested;
        }
        return null;
    }

    private void handleOnMainThread(String requestId, java.util.function.Supplier<JsonElement> handler) {
        CompletableFuture<JsonElement> future = new CompletableFuture<>();
        adapter.runOnMainThread(() -> {
            try {
                JsonElement result = handler.get();
                future.complete(result);
            } catch (Throwable e) {
                future.completeExceptionally(e);
            }
        });
        // If the main thread is wedged (e.g. the game loop stalled) the action
        // would otherwise hang forever and Takaro's request never resolves.
        future.orTimeout(10, TimeUnit.SECONDS).whenCompleteAsync((result, error) -> {
            if (error != null) {
                adapter.logWarning("Action failed: " + error.getMessage());
                sendResponse(requestId, null, error.getMessage());
            } else if (result != null) {
                sendResponse(requestId, result, null);
            } else {
                sendResponse(requestId, new JsonObject(), null);
            }
        });
    }

    // --- Action handlers ---

    private void handleTestReachability(String requestId) {
        JsonObject responsePayload = new JsonObject();
        responsePayload.addProperty("connectable", true);
        responsePayload.addProperty("reason", (String) null);
        sendResponse(requestId, responsePayload, null);
    }

    private JsonElement handleGetPlayer(JsonObject args) {
        String gameId = extractGameId(args);
        if (gameId == null) return null;
        PlayerInfo player = adapter.getPlayer(gameId);
        return player != null ? playerInfoToJson(player) : null;
    }

    private JsonElement handleGetPlayers() {
        List<PlayerInfo> players = adapter.getPlayers();
        JsonArray arr = new JsonArray();
        for (PlayerInfo p : players) {
            arr.add(playerInfoToJson(p));
        }
        return arr;
    }

    private JsonElement handleGetPlayerLocation(JsonObject args) {
        String gameId = extractGameId(args);
        if (gameId == null) return null;
        PlayerLocation loc = adapter.getPlayerLocation(gameId);
        // An offline/unknown player has no position. Returning null makes the
        // dispatcher send an empty {} payload, which Takaro rejects as an
        // invalid IPosition ("x isNumber"). Signal a proper action error instead.
        if (loc == null) throw new IllegalStateException("Player not online: " + gameId);
        JsonObject obj = new JsonObject();
        obj.addProperty("x", loc.x());
        obj.addProperty("y", loc.y());
        obj.addProperty("z", loc.z());
        if (loc.dimension() != null) obj.addProperty("dimension", loc.dimension());
        return obj;
    }

    private JsonElement handleGetPlayerInventory(JsonObject args) {
        String gameId = extractGameId(args);
        if (gameId == null) return new JsonArray();
        List<InventoryItem> items = adapter.getPlayerInventory(gameId);
        JsonArray arr = new JsonArray();
        for (InventoryItem item : items) {
            JsonObject obj = new JsonObject();
            obj.addProperty("code", item.code());
            obj.addProperty("name", item.name());
            obj.addProperty("amount", item.amount());
            obj.addProperty("quality", item.quality());
            arr.add(obj);
        }
        return arr;
    }

    private void handleGiveItem(JsonObject args) {
        String gameId = extractGameId(args);
        // Takaro sends the item identifier under itemCode (confirmed against the
        // shipped Terraria/Conan connectors); accept the known aliases.
        String itemCode = firstString(args, "itemCode", "item", "code", "name");
        int amount = optInt(args, "amount", optInt(args, "quantity", 1));
        String quality = optString(args, "quality", "");
        if (gameId != null && itemCode != null) {
            adapter.giveItem(gameId, itemCode, amount, quality);
        }
    }

    /** First present, primitive string value among the given keys, else null. */
    private static String firstString(JsonObject args, String... keys) {
        if (args == null) return null;
        for (String k : keys) {
            String v = optString(args, k, null);
            if (v != null) return v;
        }
        return null;
    }

    private JsonElement handleListItems() {
        List<GameItem> items = adapter.listItems();
        JsonArray arr = new JsonArray();
        for (GameItem item : items) {
            JsonObject obj = new JsonObject();
            obj.addProperty("code", item.code());
            obj.addProperty("name", item.name());
            obj.addProperty("description", item.description());
            arr.add(obj);
        }
        return arr;
    }

    private JsonElement handleListEntities() {
        List<GameEntity> entities = adapter.listEntities();
        JsonArray arr = new JsonArray();
        for (GameEntity entity : entities) {
            JsonObject obj = new JsonObject();
            obj.addProperty("code", entity.code());
            obj.addProperty("name", entity.name());
            obj.addProperty("description", entity.description());
            obj.addProperty("type", entity.type());
            arr.add(obj);
        }
        return arr;
    }

    private JsonElement handleListLocations() {
        List<GameLocation> locations = adapter.listLocations();
        JsonArray arr = new JsonArray();
        for (GameLocation loc : locations) {
            JsonObject obj = new JsonObject();
            obj.addProperty("name", loc.name());
            obj.addProperty("code", loc.code());
            JsonObject position = new JsonObject();
            position.addProperty("x", loc.x());
            position.addProperty("y", loc.y());
            position.addProperty("z", loc.z());
            if (loc.dimension() != null) position.addProperty("dimension", loc.dimension());
            obj.add("position", position);
            if (loc.radius() != null) obj.addProperty("radius", loc.radius());
            if (loc.sizeX() != null) obj.addProperty("sizeX", loc.sizeX());
            if (loc.sizeY() != null) obj.addProperty("sizeY", loc.sizeY());
            if (loc.sizeZ() != null) obj.addProperty("sizeZ", loc.sizeZ());
            arr.add(obj);
        }
        return arr;
    }

    private JsonElement handleExecuteConsoleCommand(JsonObject args) {
        String command = optString(args, "command", "");
        CommandResult result = adapter.executeConsoleCommand(command);
        JsonObject obj = new JsonObject();
        obj.addProperty("success", result.success());
        obj.addProperty("rawResult", result.rawResult());
        obj.addProperty("errorMessage", result.errorMessage());
        return obj;
    }

    private void handleSendMessage(JsonObject args) {
        String message = optString(args, "message", "");
        String recipientGameId = null;
        String senderName = null;
        JsonObject opts = optObject(args, "opts");
        if (opts != null) {
            recipientGameId = optString(optObject(opts, "recipient"), "gameId", null);
            senderName = optString(opts, "senderNameOverride", null);
        }
        // PZ server chat shows no author for a plain message, so prefix a name:
        // Takaro's senderNameOverride when provided, otherwise "Server".
        if (senderName == null || senderName.isEmpty()) {
            String configured = config.getServerChatName();
            String serverName = adapter.getServerName();
            if (configured != null && !configured.isEmpty()) {
                senderName = configured;
            } else if (serverName != null && !serverName.isEmpty()) {
                senderName = serverName;
            } else {
                senderName = "Server";
            }
        }
        adapter.sendMessage(senderName + ": " + message, recipientGameId);
    }

    private void handleTeleportPlayer(JsonObject args) {
        String gameId = extractGameId(args);
        double x = optDouble(args, "x", 0);
        double y = optDouble(args, "y", 0);
        double z = optDouble(args, "z", 0);
        String dimension = optString(args, "dimension", null);
        if (gameId != null) {
            adapter.teleportPlayer(gameId, x, y, z, dimension);
        }
    }

    private void handleKickPlayer(JsonObject args) {
        String gameId = extractGameId(args);
        String reason = optString(args, "reason", "");
        if (gameId != null) {
            adapter.kickPlayer(gameId, reason);
        }
    }

    private void handleBanPlayer(JsonObject args) {
        String gameId = extractGameId(args);
        String reason = optString(args, "reason", "");
        String expiresAt = optString(args, "expiresAt", null);
        if (gameId != null) {
            adapter.banPlayer(gameId, reason, expiresAt);
        }
    }

    private void handleUnbanPlayer(JsonObject args) {
        String gameId = extractGameId(args);
        if (gameId != null) {
            adapter.unbanPlayer(gameId);
        }
    }

    private JsonElement handleListBans() {
        List<BanEntry> bans = adapter.listBans();
        JsonArray arr = new JsonArray();
        for (BanEntry ban : bans) {
            JsonObject obj = new JsonObject();
            JsonObject player = new JsonObject();
            player.addProperty("gameId", ban.gameId());
            player.addProperty("name", ban.name());
            obj.add("player", player);
            obj.addProperty("reason", ban.reason());
            obj.addProperty("expiresAt", ban.expiresAt());
            arr.add(obj);
        }
        return arr;
    }

    // --- JSON helpers ---

    private JsonObject playerInfoToJson(PlayerInfo p) {
        JsonObject obj = new JsonObject();
        obj.addProperty("gameId", p.gameId());
        obj.addProperty("name", p.name());
        obj.addProperty("steamId", p.steamId());
        obj.addProperty("epicOnlineServicesId", p.epicOnlineServicesId());
        obj.addProperty("xboxLiveId", p.xboxLiveId());
        obj.addProperty("platformId", p.platformId());
        obj.addProperty("ip", p.ip());
        obj.addProperty("ping", p.ping());
        return obj;
    }

    // --- Response helpers ---

    private void sendResponse(String requestId, JsonElement payload, String error) {
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "response");
        msg.addProperty("requestId", requestId);

        if (error != null) {
            msg.addProperty("error", error);
        } else if (payload != null) {
            msg.add("payload", payload);
        }

        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND response (requestId=" + requestId + "): " + msg);
        }
        send(msg.toString());
    }

    private void scheduleReconnect() {
        adapter.logInfo("Reconnecting in " + (currentReconnectDelay / 1000) + "s...");
        scheduler.schedule(() -> {
            if (!shouldReconnect) return;
            try {
                reconnectBlocking();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }, currentReconnectDelay, TimeUnit.MILLISECONDS);

        currentReconnectDelay = Math.min(
                (long) (currentReconnectDelay * config.getBackoffMultiplier()),
                config.getMaxReconnectDelay()
        );
    }
}
