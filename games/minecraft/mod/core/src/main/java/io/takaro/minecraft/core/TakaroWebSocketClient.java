package io.takaro.minecraft.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.map.WorldMap;
import io.takaro.minecraft.core.model.*;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.Base64;
import java.util.List;
import java.util.Map;
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
    private volatile boolean closed;
    private final String configPath;
    private final EventOutbox outbox;
    private final WorldMap worldMap;
    // The protocol Takaro chose in the last identifyResponse; 0 until then and on every reconnect.
    private volatile int protocol;

    // Takaro polls every connected server several times a minute. A socket that stays open
    // but goes quiet has been dropped on Takaro's side (seen on staging 2026-10-12: no close
    // frame, the ingress kept answering pings), so reconnect instead of waiting forever.
    static final long SILENCE_LIMIT_MILLIS = 180_000;
    private volatile long silenceLimitMillis = SILENCE_LIMIT_MILLIS;
    private volatile long lastInboundMillis = System.currentTimeMillis();
    private volatile boolean watchdogStarted;

    /** The v1 action capabilities this connector implements; testReachability needs none. */
    static final String[] ACTION_CAPABILITIES = {
            "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
            "items.list", "items.give", "entities.list", "locations.list", "chat.send", "console.execute",
            "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown"};
    static final String[] EVENT_TYPES = {
            "log", "player-connected", "player-disconnected", "chat-message", "player-death", "entity-killed"};
    // Takaro gives a connector 10 s per request; leave room for the round trip.
    static final long MAP_TILE_WAIT_MILLIS = 6000;

    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config) {
        this(serverUri, adapter, config, "the Takaro config file");
    }

    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config, String configPath) {
        this(serverUri, adapter, config, configPath, new EventOutbox(), null);
    }

    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config, String configPath,
                                 EventOutbox outbox, WorldMap worldMap) {
        super(serverUri);
        this.adapter = adapter;
        this.config = config;
        this.configPath = configPath;
        this.outbox = outbox;
        this.worldMap = worldMap;
        this.currentReconnectDelay = config.getReconnectDelay();
    }

    private synchronized void startWatchdog() {
        if (watchdogStarted) return;
        watchdogStarted = true;
        long period = Math.max(1000, Math.min(15_000, silenceLimitMillis / 4));
        scheduler.scheduleWithFixedDelay(this::checkSilence, period, period, TimeUnit.MILLISECONDS);
    }

    /** One watchdog tick: drops a connection Takaro has not spoken on for too long. */
    void checkSilence() {
        try {
            long quiet = System.currentTimeMillis() - lastInboundMillis;
            if (closed || !identified || !isOpen() || quiet < silenceLimitMillis) return;
            adapter.logWarning("No message from Takaro for " + quiet / 1000 + " s on an open connection; reconnecting");
            identified = false;
            dropConnection();
        } catch (RuntimeException e) {
            adapter.logWarning("Connection watchdog failed: " + e);
        }
    }

    /** Closes the socket at once, without waiting for a close handshake a dead peer never sends. */
    void dropConnection() {
        getConnection().closeConnection(1001, "no traffic from Takaro");
    }

    void setSilenceLimitMillis(long millis) {
        this.silenceLimitMillis = millis;
    }

    /** True once Takaro accepted protocol v1 on this connection. */
    boolean isV1() {
        return protocol >= 1;
    }

    /** The settings this connection uses; the connector updates the ones that need no reconnect. */
    TakaroConfig settings() {
        return config;
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        if (closed) {
            // A reconnect that was already under way when a config change replaced this client.
            close();
            return;
        }
        protocol = 0;
        identified = false;
        lastInboundMillis = System.currentTimeMillis();
        startWatchdog();
        adapter.logInfo("WebSocket connected, sending identify...");
        sendIdentify();
    }

    @Override
    public void onMessage(String message) {
        if (closed) {
            return; // a replaced connection: its answers no longer describe the current settings
        }
        lastInboundMillis = System.currentTimeMillis();
        if (config.isDebugEnabled()) {
            // identify errors can embed Takaro's internal request, auth token included
            adapter.logDebug(message.contains("\"identifyResponse\"")
                    ? "WS RECV: identifyResponse (body not logged)" : "WS RECV: " + message);
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
                case "eventAck": {
                    JsonObject ack = optObject(json, "payload");
                    if (ack != null && hasValue(ack, "upTo")) {
                        outbox.ack(optString(ack, "streamId", ""), ack.get("upTo").getAsLong());
                    }
                    break;
                }
                case "eventError": {
                    JsonObject err = optObject(json, "payload");
                    JsonObject detail = optObject(err, "error");
                    adapter.logWarning("Takaro rejected event seq " + optString(err, "seq", "?") + ": "
                            + optString(detail, "code", "?") + " " + optString(detail, "message", ""));
                    break;
                }
                case "capabilitiesResponse":
                    if (hasValue(json, "error")) {
                        adapter.logWarning("Takaro refused the capability update: " + json.get("error"));
                    } else {
                        adapter.logInfo("Takaro accepted the capability update");
                    }
                    break;
                case "error":
                    String errorMsg = "unknown";
                    JsonObject errorPayload = optObject(json, "payload");
                    if (errorPayload != null && hasValue(errorPayload, "message")) {
                        errorMsg = optString(errorPayload, "message", "unknown");
                    } else if (hasValue(json, "message")) {
                        errorMsg = optString(json, "message", "unknown");
                    }
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
        identified = false;
        protocol = 0;
        adapter.logInfo("WebSocket closed (code=" + code + ", reason=" + reason + ", remote=" + remote + ")");

        if (!closed && (code == 1008 || code == 4001 || code == 4003)) {
            shouldReconnect = false;
            TakaroConnector.logBanner(adapter,
                    "Takaro refused the connection (" + (reason == null || reason.isEmpty() ? "code " + code : reason) + ").",
                    "Check registration_token and identity_token in " + configPath + ".",
                    "Saving a corrected token reconnects within a few seconds, no restart needed.");
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
        closed = true;
        shouldReconnect = false;
        scheduler.shutdownNow();
        try {
            // sends are queued and written by the websocket writer thread; give any
            // pending frames (e.g. player-disconnected on server stop) a bounded
            // chance to leave the socket before we close it.
            long deadline = System.currentTimeMillis() + 2000L;
            while (isOpen() && hasBufferedData() && System.currentTimeMillis() < deadline) {
                Thread.sleep(20);
            }
            // Not closeBlocking(): it waits forever on a client that is between reconnect
            // attempts, which would hang the server stop or a config change.
            close();
            deadline = System.currentTimeMillis() + 2000L;
            while (!isClosed() && getReadyState() != org.java_websocket.enums.ReadyState.NOT_YET_CONNECTED
                    && System.currentTimeMillis() < deadline) {
                Thread.sleep(20);
            }
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
        if (config.isMapEnabled()) {
            // v1 is on offer: keep every event until Takaro acks it, and send it once identified.
            JsonObject payload = outbox.add(eventType, data);
            if (isV1() && isOpen()) {
                sendEventPayload(payload);
                return;
            }
            if (protocol == 0 && isOpen() && identified) {
                // Takaro chose v0: plain events, nothing to ack.
                outbox.ack(outbox.streamId(), payload.get("seq").getAsLong());
                JsonObject plain = new JsonObject();
                plain.addProperty("type", eventType);
                plain.add("data", data);
                sendEventPayload(plain);
            }
            return;
        }
        if (!isOpen()) return;

        JsonObject payload = new JsonObject();
        payload.addProperty("type", eventType);
        payload.add("data", data);
        sendEventPayload(payload);
    }

    private void sendEventPayload(JsonObject payload) {
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "gameEvent");
        msg.add("payload", payload);

        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND gameEvent: " + msg);
        }
        send(msg.toString());
    }

    /** Replaces the capability manifest, for example when the map bounds grew. */
    void sendCapabilities() {
        if (!isV1() || !isOpen()) return;
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "capabilities");
        msg.addProperty("requestId", java.util.UUID.randomUUID().toString());
        JsonObject payload = new JsonObject();
        payload.add("capabilities", capabilities());
        msg.add("payload", payload);
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND capabilities: " + msg);
        }
        send(msg.toString());
    }

    JsonArray capabilities() {
        JsonArray caps = new JsonArray();
        for (String name : ACTION_CAPABILITIES) {
            caps.add(capability(name));
        }
        for (String event : EVENT_TYPES) {
            caps.add(capability("events." + event));
        }
        if (worldMap != null) {
            JsonObject map = capability("map");
            JsonObject parameters = new JsonObject();
            parameters.add("dimensions", GSON.toJsonTree(worldMap.declaration()));
            map.add("parameters", parameters);
            caps.add(map);
        }
        return caps;
    }

    private static JsonObject capability(String name) {
        JsonObject cap = new JsonObject();
        cap.addProperty("name", name);
        return cap;
    }

    private static final com.google.gson.Gson GSON = new com.google.gson.Gson();
    private volatile boolean identified;

    private void sendIdentify() {
        JsonObject payload = new JsonObject();
        String identity = config.getIdentityToken();
        String registration = config.getRegistrationToken();
        payload.addProperty("identityToken", identity != null ? identity : "");
        payload.addProperty("registrationToken", registration != null ? registration : "");
        if (config.isMapEnabled()) {
            JsonArray versions = new JsonArray();
            versions.add(0);
            versions.add(1);
            payload.add("protocolVersions", versions);
            payload.add("capabilities", capabilities());
        }

        JsonObject msg = new JsonObject();
        msg.addProperty("type", "identify");
        msg.add("payload", payload);
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND identify (tokens redacted)");
        }
        send(msg.toString());
    }

    private void handleIdentifyResponse(JsonObject json) {
        JsonObject payloadOrNull = optObject(json, "payload");
        JsonObject payload = payloadOrNull != null ? payloadOrNull : new JsonObject();

        // v0 puts the error in the payload; v1 sends a null payload and a top-level error.
        JsonElement errorElement = hasValue(payload, "error") ? payload.get("error")
                : hasValue(json, "error") ? json.get("error") : null;
        if (errorElement != null) {
            String errorMessage;
            if (errorElement.isJsonObject()) {
                // Only name, message and status: the full error can carry Takaro's request headers.
                JsonObject errorObj = errorElement.getAsJsonObject();
                String name = optString(errorObj, "name", "");
                errorMessage = optString(errorObj, "message", name.isEmpty() ? "unknown error" : name);
                JsonElement http = errorObj.get("http");
                if (http != null && http.isJsonPrimitive()) {
                    errorMessage += " (HTTP " + http.getAsString() + ")";
                }
            } else {
                errorMessage = errorElement.getAsString();
            }
            if (errorMessage.endsWith("(HTTP 409)")) {
                // Takaro names a new server after its identity, and names are unique per domain.
                TakaroConnector.logBanner(adapter,
                        "Identify failed: " + errorMessage,
                        "Takaro already has a game server with this name. Set a different",
                        "identity_token in " + configPath + " and save it, or remove the old server in Takaro.");
                close();
                return;
            }
            TakaroConnector.logBanner(adapter,
                    "Identify failed: " + errorMessage,
                    "Check registration_token in " + configPath + ". Saving a corrected",
                    "token reconnects within a few seconds; until then the connector retries with backoff.");
            // Close so the reconnect timer retries: a rejection can also be a passing Takaro outage.
            close();
            return;
        }

        currentReconnectDelay = config.getReconnectDelay();
        protocol = optInt(payload, "protocolVersion", 0);
        identified = true;

        JsonObject server = optObject(payload, "server");
        String serverId = server != null && hasValue(server, "id") ? optString(server, "id", "")
                : optString(payload, "gameServerId", null);
        adapter.logInfo("Identified successfully" + (serverId != null ? ", server ID: " + serverId : "")
                + (config.isMapEnabled() ? ", protocol v" + protocol : ""));
        if (isV1()) {
            List<JsonObject> backlog = outbox.unacked();
            if (!backlog.isEmpty()) {
                adapter.logInfo("Resending " + backlog.size() + " events Takaro has not acknowledged");
            }
            for (JsonObject event : backlog) {
                sendEventPayload(event);
            }
        }
    }

    private void handleRequest(JsonObject json) {
        String requestId = optString(json, "requestId", null);
        if (requestId == null) {
            adapter.logWarning("Received request without requestId");
            return;
        }

        JsonObject reqPayload = optObject(json, "payload");
        JsonObject payload = reqPayload != null ? reqPayload : new JsonObject();
        String action = optString(payload, "action", "");
        if (config.isDebugEnabled()) {
            adapter.logDebug("Request: action=" + action + ", requestId=" + requestId);
        }

        // Parse args - it's a JSON string that needs to be parsed
        JsonElement argsElement = hasValue(payload, "args") ? payload.get("args") : null;
        JsonObject args;
        if (argsElement != null && argsElement.isJsonPrimitive() && argsElement.getAsJsonPrimitive().isString()) {
            String argsStr = argsElement.getAsString();
            args = argsStr.isEmpty() ? new JsonObject() : JsonParser.parseString(argsStr).getAsJsonObject();
        } else if (argsElement != null && argsElement.isJsonObject()) {
            args = argsElement.getAsJsonObject();
        } else {
            args = new JsonObject();
        }

        if (action.equals("getMapTile") && isV1()) {
            handleGetMapTile(requestId, args);
            return;
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
                sendError(requestId, "unsupported", "Action not implemented: " + action);
                break;
        }
    }

    private void handleGetMapTile(String requestId, JsonObject args) {
        if (worldMap == null) {
            sendError(requestId, "unsupported", "the world map is switched off");
            return;
        }
        String dimension = optString(args, "dimension", null);
        if (dimension == null || !hasValue(args, "zoom") || !hasValue(args, "x") || !hasValue(args, "y")) {
            sendError(requestId, "invalid_args", "getMapTile needs dimension, zoom, x and y");
            return;
        }
        int zoom = optInt(args, "zoom", -1), x = optInt(args, "x", -1), y = optInt(args, "y", -1);
        // Off the websocket thread: a cold tile reads region files for a few seconds.
        CompletableFuture.runAsync(() -> {
            try {
                byte[] png = worldMap.tile(dimension, zoom, x, y, MAP_TILE_WAIT_MILLIS);
                JsonObject tile = new JsonObject();
                tile.addProperty("mimeType", "image/png");
                tile.addProperty("data", Base64.getEncoder().encodeToString(png));
                sendResponse(requestId, tile, null);
            } catch (WorldMap.NotReadyException e) {
                sendError(requestId, "not_found", e.getMessage());
            } catch (IllegalArgumentException e) {
                sendError(requestId, "invalid_args", e.getMessage());
            } catch (RuntimeException e) {
                adapter.logWarning("Map tile failed: " + e);
                sendError(requestId, "game_error", "map tile failed: " + e.getMessage());
            }
        });
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
                Throwable cause = error instanceof java.util.concurrent.CompletionException && error.getCause() != null
                        ? error.getCause() : error;
                sendError(requestId, cause instanceof IllegalArgumentException ? "invalid_args" : "game_error",
                        cause.getMessage());
            } else if (result != null) {
                sendResponse(requestId, result, null);
            } else if (isV1()) {
                sendResponse(requestId, com.google.gson.JsonNull.INSTANCE, null);
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
        String gameId = optString(args, "gameId", null);
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
        String gameId = optString(args, "gameId", null);
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
        String gameId = optString(args, "gameId", null);
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
        JsonObject player = optObject(args, "player");
        String gameId = optString(player, "gameId", null);
        String itemCode = optString(args, "item", null);
        int amount = optInt(args, "amount", 1);
        String quality = optString(args, "quality", "");
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
        JsonObject opts = optObject(args, "opts");
        if (opts != null) {
            JsonObject recipient = optObject(opts, "recipient");
            if (recipient != null) {
                recipientGameId = optString(recipient, "gameId", null);
            }
        }
        adapter.sendMessage(message, recipientGameId);
    }

    private void handleTeleportPlayer(JsonObject args) {
        JsonObject player = optObject(args, "player");
        String gameId = optString(player, "gameId", null);
        double x = optDouble(args, "x", 0);
        double y = optDouble(args, "y", 0);
        double z = optDouble(args, "z", 0);
        String dimension = optString(args, "dimension", null);
        if (gameId != null) {
            adapter.teleportPlayer(gameId, x, y, z, dimension);
        }
    }

    private void handleKickPlayer(JsonObject args) {
        JsonObject player = optObject(args, "player");
        String gameId = optString(player, "gameId", null);
        String reason = optString(args, "reason", "");
        if (gameId != null) {
            adapter.kickPlayer(gameId, reason);
        }
    }

    private void handleBanPlayer(JsonObject args) {
        JsonObject player = optObject(args, "player");
        String gameId = optString(player, "gameId", null);
        String reason = optString(args, "reason", "");
        String expiresAt = optString(args, "expiresAt", null);
        if (gameId != null) {
            adapter.banPlayer(gameId, reason, expiresAt);
        }
    }

    private void handleUnbanPlayer(JsonObject args) {
        String gameId = optString(args, "gameId", null);
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

    // --- Null-safe argument accessors ---
    // Takaro may send optional fields as JSON null (e.g. shop deliveries send
    // "quality": null); getAsString() on JsonNull throws, so always go through these.

    private static boolean hasValue(JsonObject obj, String key) {
        return obj != null && obj.has(key) && !obj.get(key).isJsonNull();
    }

    private static JsonObject optObject(JsonObject obj, String key) {
        if (!hasValue(obj, key)) return null;
        JsonElement el = obj.get(key);
        return el.isJsonObject() ? el.getAsJsonObject() : null;
    }

    private static String optString(JsonObject obj, String key, String fallback) {
        if (!hasValue(obj, key)) return fallback;
        JsonElement el = obj.get(key);
        return el.isJsonPrimitive() ? el.getAsString() : el.toString();
    }

    private static int optInt(JsonObject obj, String key, int fallback) {
        if (!hasValue(obj, key)) return fallback;
        try {
            return obj.get(key).getAsInt();
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    private static double optDouble(JsonObject obj, String key, double fallback) {
        if (!hasValue(obj, key)) return fallback;
        try {
            return obj.get(key).getAsDouble();
        } catch (RuntimeException e) {
            return fallback;
        }
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

    /** v1 errors carry a code Takaro understands; v0 keeps its free-text error. */
    private void sendError(String requestId, String code, String message) {
        if (!isV1()) {
            sendResponse(requestId, null, message);
            return;
        }
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "response");
        msg.addProperty("requestId", requestId);
        JsonObject error = new JsonObject();
        error.addProperty("code", code);
        error.addProperty("message", message == null || message.isEmpty() ? code : message);
        msg.add("error", error);
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND response (requestId=" + requestId + "): " + msg);
        }
        send(msg.toString());
    }

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
