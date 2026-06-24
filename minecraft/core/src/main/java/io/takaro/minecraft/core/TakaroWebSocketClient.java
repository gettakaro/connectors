package io.takaro.minecraft.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.model.*;
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
    }

    @Override
    public void onMessage(String message) {
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS RECV: " + message);
        }
        try {
            JsonObject json = JsonParser.parseString(message).getAsJsonObject();
            String type = json.has("type") ? json.get("type").getAsString() : "";

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
                case "error":
                    String errorMsg = "unknown";
                    if (json.has("payload") && json.getAsJsonObject("payload").has("message")) {
                        errorMsg = json.getAsJsonObject("payload").get("message").getAsString();
                    } else if (json.has("message")) {
                        errorMsg = json.get("message").getAsString();
                    }
                    String requestId = json.has("requestId") ? json.get("requestId").getAsString() : null;
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
    public void emitPlayerDisconnected(String gameId, String playerName) {
        JsonObject data = new JsonObject();
        JsonObject player = new JsonObject();
        player.addProperty("gameId", gameId);
        player.addProperty("name", playerName);
        data.add("player", player);
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
        JsonObject payload = json.has("payload") ? json.getAsJsonObject("payload") : new JsonObject();

        if (payload.has("error") && !payload.get("error").isJsonNull()) {
            JsonElement errorElement = payload.get("error");
            String errorMessage;
            if (errorElement.isJsonObject()) {
                JsonObject errorObj = errorElement.getAsJsonObject();
                errorMessage = errorObj.has("message") ? errorObj.get("message").getAsString() : errorObj.toString();
            } else {
                errorMessage = errorElement.getAsString();
            }
            adapter.logWarning("Identify failed: " + errorMessage);
            return;
        }

        currentReconnectDelay = config.getReconnectDelay();

        if (payload.has("server") && payload.getAsJsonObject("server").has("id")) {
            String serverId = payload.getAsJsonObject("server").get("id").getAsString();
            adapter.logInfo("Identified successfully, server ID: " + serverId);
        } else {
            adapter.logInfo("Identified successfully");
        }
    }

    private void handleRequest(JsonObject json) {
        String requestId = json.has("requestId") ? json.get("requestId").getAsString() : null;
        if (requestId == null) {
            adapter.logWarning("Received request without requestId");
            return;
        }

        JsonObject payload = json.has("payload") ? json.getAsJsonObject("payload") : new JsonObject();
        String action = payload.has("action") ? payload.get("action").getAsString() : "";
        if (config.isDebugEnabled()) {
            adapter.logDebug("Request: action=" + action + ", requestId=" + requestId);
        }

        // Parse args - it's a JSON string that needs to be parsed
        JsonElement argsElement = payload.has("args") ? payload.get("args") : null;
        JsonObject args;
        if (argsElement != null && argsElement.isJsonPrimitive() && argsElement.getAsJsonPrimitive().isString()) {
            String argsStr = argsElement.getAsString();
            args = argsStr.isEmpty() ? new JsonObject() : JsonParser.parseString(argsStr).getAsJsonObject();
        } else if (argsElement != null && argsElement.isJsonObject()) {
            args = argsElement.getAsJsonObject();
        } else {
            args = new JsonObject();
        }

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

    private void handleOnMainThread(String requestId, java.util.function.Supplier<JsonElement> handler) {
        CompletableFuture<JsonElement> future = new CompletableFuture<>();
        adapter.runOnMainThread(() -> {
            try {
                JsonElement result = handler.get();
                future.complete(result);
            } catch (Exception e) {
                future.completeExceptionally(e);
            }
        });
        future.whenCompleteAsync((result, error) -> {
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
        String gameId = args.has("gameId") ? args.get("gameId").getAsString() : null;
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
        String gameId = args.has("gameId") ? args.get("gameId").getAsString() : null;
        if (gameId == null) return null;
        PlayerLocation loc = adapter.getPlayerLocation(gameId);
        if (loc == null) return null;
        JsonObject obj = new JsonObject();
        obj.addProperty("x", loc.x());
        obj.addProperty("y", loc.y());
        obj.addProperty("z", loc.z());
        if (loc.dimension() != null) obj.addProperty("dimension", loc.dimension());
        return obj;
    }

    private JsonElement handleGetPlayerInventory(JsonObject args) {
        String gameId = args.has("gameId") ? args.get("gameId").getAsString() : null;
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
        JsonObject player = args.has("player") ? args.getAsJsonObject("player") : null;
        String gameId = player != null && player.has("gameId") ? player.get("gameId").getAsString() : null;
        String itemCode = args.has("item") ? args.get("item").getAsString() : null;
        int amount = args.has("amount") ? args.get("amount").getAsInt() : 1;
        String quality = args.has("quality") ? args.get("quality").getAsString() : "";
        if (gameId != null && itemCode != null) {
            adapter.giveItem(gameId, itemCode, amount, quality);
        }
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
        String command = args.has("command") ? args.get("command").getAsString() : "";
        CommandResult result = adapter.executeConsoleCommand(command);
        JsonObject obj = new JsonObject();
        obj.addProperty("success", result.success());
        obj.addProperty("rawResult", result.rawResult());
        obj.addProperty("errorMessage", result.errorMessage());
        return obj;
    }

    private void handleSendMessage(JsonObject args) {
        String message = args.has("message") ? args.get("message").getAsString() : "";
        String recipientGameId = null;
        if (args.has("opts") && args.get("opts").isJsonObject()) {
            JsonObject opts = args.getAsJsonObject("opts");
            if (opts.has("recipient") && opts.get("recipient").isJsonObject()) {
                JsonObject recipient = opts.getAsJsonObject("recipient");
                recipientGameId = recipient.has("gameId") ? recipient.get("gameId").getAsString() : null;
            }
        }
        adapter.sendMessage(message, recipientGameId);
    }

    private void handleTeleportPlayer(JsonObject args) {
        JsonObject player = args.has("player") ? args.getAsJsonObject("player") : null;
        String gameId = player != null && player.has("gameId") ? player.get("gameId").getAsString() : null;
        double x = args.has("x") ? args.get("x").getAsDouble() : 0;
        double y = args.has("y") ? args.get("y").getAsDouble() : 0;
        double z = args.has("z") ? args.get("z").getAsDouble() : 0;
        String dimension = args.has("dimension") ? args.get("dimension").getAsString() : null;
        if (gameId != null) {
            adapter.teleportPlayer(gameId, x, y, z, dimension);
        }
    }

    private void handleKickPlayer(JsonObject args) {
        JsonObject player = args.has("player") ? args.getAsJsonObject("player") : null;
        String gameId = player != null && player.has("gameId") ? player.get("gameId").getAsString() : null;
        String reason = args.has("reason") ? args.get("reason").getAsString() : "";
        if (gameId != null) {
            adapter.kickPlayer(gameId, reason);
        }
    }

    private void handleBanPlayer(JsonObject args) {
        JsonObject player = args.has("player") ? args.getAsJsonObject("player") : null;
        String gameId = player != null && player.has("gameId") ? player.get("gameId").getAsString() : null;
        String reason = args.has("reason") ? args.get("reason").getAsString() : "";
        String expiresAt = args.has("expiresAt") ? args.get("expiresAt").getAsString() : null;
        if (gameId != null) {
            adapter.banPlayer(gameId, reason, expiresAt);
        }
    }

    private void handleUnbanPlayer(JsonObject args) {
        String gameId = args.has("gameId") ? args.get("gameId").getAsString() : null;
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
