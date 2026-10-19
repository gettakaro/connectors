package io.takaro.zomboid.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.Base64;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

public class TakaroWebSocketClient extends WebSocketClient implements EventEmitter {

    /** Largest getMapImage chunk the protocol allows (1 MiB). */
    static final int MAX_MAP_CHUNK = 1024 * 1024;
    /** Debug lines longer than this are cut (a map chunk response is ~700 KB of base64). */
    private static final int MAX_DEBUG_LINE = 2000;

    private final GameAdapter adapter;
    private final TakaroConfig config;
    private final EventOutbox outbox;
    // Per connection: set by a successful identifyResponse, cleared when the socket opens or closes.
    private volatile boolean identified = false;
    private volatile int protocolVersion = 0;
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "takaro-reconnect");
        t.setDaemon(true);
        return t;
    });
    private volatile long currentReconnectDelay;
    private volatile boolean shouldReconnect = true;
    // Set once a config change or shutdown replaced this client: it must not reconnect,
    // identify or hand events to the game any more, whatever its socket thread still does.
    private volatile boolean retired = false;

    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config) {
        this(serverUri, adapter, config, new EventOutbox());
    }

    /** {@code outbox} outlives this client, so a replacement socket keeps the event stream. */
    public TakaroWebSocketClient(URI serverUri, GameAdapter adapter, TakaroConfig config, EventOutbox outbox) {
        super(serverUri);
        this.adapter = adapter;
        this.config = config;
        this.outbox = outbox;
        this.currentReconnectDelay = config.getReconnectDelay();
    }

    /** The protocol version negotiated on the current connection (0 until identified). */
    public int protocolVersion() {
        return protocolVersion;
    }

    public boolean isIdentified() {
        return identified;
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        if (retired) {
            close();
            return;
        }
        identified = false;
        protocolVersion = 0;
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
        if (retired) {
            return;
        }
        if (config.isDebugEnabled()) {
            // An identify error can carry Takaro's internal request, auth token included.
            adapter.logDebug(message.contains("\"identifyResponse\"")
                    ? "WS RECV identifyResponse (not logged raw)"
                    : "WS RECV: " + message);
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
                case "eventAck":
                    handleEventAck(json);
                    break;
                case "eventError":
                    handleEventError(json);
                    break;
                case "capabilitiesResponse":
                    handleCapabilitiesResponse(json);
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
        identified = false;
        if (retired) {
            return;
        }
        adapter.logInfo("WebSocket closed (code=" + code + ", reason=" + reason + ", remote=" + remote + ")");

        if (code == 1008 || code == 4001 || code == 4003) {
            // A new token in the config file replaces this client, which reconnects.
            TakaroConnector.logBanner(adapter,
                    "Takaro refused the connection (code " + code + (reason != null && !reason.isEmpty() ? ", " + reason : "") + ").",
                    "Check registrationToken in " + config.getConfigFileHint() + ". Saving a corrected",
                    "token reconnects within a few seconds, no restart needed.");
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

    /** Stops this client without waiting for its socket; used when a config change replaces it. */
    public void retire() {
        retired = true;
        shouldReconnect = false;
        scheduler.shutdownNow();
        close();
    }

    public void shutdown() {
        retired = true;
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
    public void emitChatMessage(PlayerInfo player, String channel, String message) {
        JsonObject data = new JsonObject();
        data.add("player", playerIdentityToJson(player));
        data.addProperty("channel", channel);
        data.addProperty("msg", message);
        sendGameEvent("chat-message", data);
    }

    @Override
    public void emitPlayerDeath(PlayerInfo player, PlayerInfo attacker, double x, double y, double z, String dimension) {
        JsonObject data = new JsonObject();
        data.add("player", playerIdentityToJson(player));

        if (attacker != null) {
            data.add("attacker", playerIdentityToJson(attacker));
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
    public void emitEntityKilled(PlayerInfo player, String entityCode, String weaponCode) {
        JsonObject data = new JsonObject();
        data.add("player", playerIdentityToJson(player));
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

    @Override
    public void capabilitiesChanged() {
        if (identified && protocolVersion >= 1 && isOpen()) {
            sendCapabilities();
        }
    }

    /**
     * Protocol 0 sends an event straight away (lost while the socket is down). With protocol 1
     * offered, every event goes through the {@link EventOutbox}: sent at once on an identified
     * protocol 1 socket, otherwise held until the next identify, which resends everything Takaro
     * has not acknowledged (or, if Takaro picked protocol 0, flushes it in the old shape).
     */
    private void sendGameEvent(String eventType, JsonObject data) {
        if (!config.isProtocolV1Enabled()) {
            if (isOpen()) sendEventFrame(v0EventPayload(eventType, data));
            return;
        }
        synchronized (outbox) {
            if (identified && protocolVersion == 0) {
                if (isOpen()) sendEventFrame(v0EventPayload(eventType, data));
                return;
            }
            EventOutbox.Pending event = outbox.add(eventType, data);
            if (identified && isOpen()) {
                sendEventFrame(event.payload());
            }
        }
    }

    private static JsonObject v0EventPayload(String eventType, JsonObject data) {
        JsonObject payload = new JsonObject();
        payload.addProperty("type", eventType);
        payload.add("data", data);
        return payload;
    }

    private void sendEventFrame(JsonObject payload) {
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "gameEvent");
        msg.add("payload", payload);

        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND gameEvent: " + msg);
        }
        send(msg.toString());
    }

    /** After identify: resend what Takaro has not acknowledged, in seq order. */
    private void flushOutbox() {
        synchronized (outbox) {
            if (!isOpen()) return;
            if (protocolVersion >= 1) {
                List<EventOutbox.Pending> pending = outbox.unacknowledged();
                if (!pending.isEmpty()) {
                    adapter.logInfo("Sending " + pending.size() + " buffered event(s) to Takaro");
                }
                for (EventOutbox.Pending event : pending) {
                    sendEventFrame(event.payload());
                }
            } else {
                for (EventOutbox.Pending event : outbox.drain()) {
                    JsonObject payload = event.payload();
                    sendEventFrame(v0EventPayload(payload.get("type").getAsString(), payload.getAsJsonObject("data")));
                }
            }
        }
    }

    private void handleEventAck(JsonObject json) {
        JsonObject payload = optObject(json, "payload");
        String streamId = optString(payload, "streamId", null);
        long upTo = (long) optDouble(payload, "upTo", -1);
        if (streamId == null || upTo < 0) return;
        outbox.acknowledge(streamId, upTo);
    }

    private void handleEventError(JsonObject json) {
        JsonObject payload = optObject(json, "payload");
        JsonObject error = optObject(json, "error");
        adapter.logWarning("Takaro rejected event seq=" + optString(payload, "seq", "?")
                + ": " + optString(error, "code", "?") + " " + optString(error, "message", ""));
    }

    private void handleCapabilitiesResponse(JsonObject json) {
        JsonObject error = optObject(json, "error");
        if (error != null) {
            adapter.logWarning("Takaro refused the capability manifest: " + optString(error, "code", "?")
                    + " " + optString(error, "message", ""));
        } else {
            adapter.logInfo("Takaro stored the capability manifest");
        }
    }

    private List<io.takaro.zomboid.core.model.MapDimension> mapDimensions() {
        try {
            List<io.takaro.zomboid.core.model.MapDimension> dims = adapter.getMapDimensions();
            return dims != null ? dims : List.of();
        } catch (RuntimeException e) {
            adapter.logWarning("Map unavailable: " + e.getMessage());
            return List.of();
        }
    }

    private String gameVersion() {
        try {
            return adapter.getGameVersion();
        } catch (RuntimeException e) {
            adapter.logWarning("Game version unavailable: " + e.getMessage());
            return null;
        }
    }

    private void sendCapabilities() {
        List<io.takaro.zomboid.core.model.MapDimension> dims = mapDimensions();
        JsonObject payload = new JsonObject();
        payload.add("capabilities", Capabilities.manifest(dims, gameVersion()));
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "capabilities");
        msg.addProperty("requestId", UUID.randomUUID().toString());
        msg.add("payload", payload);
        adapter.logInfo("Sending capability manifest (map dimensions: " + dims.size() + ")");
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND " + clip(msg.toString()));
        }
        send(msg.toString());
    }

    private void sendIdentify() {
        JsonObject payload = new JsonObject();
        String identity = config.getIdentityToken();
        String registration = config.getRegistrationToken();
        payload.addProperty("identityToken", identity != null ? identity : "");
        payload.addProperty("registrationToken", registration != null ? registration : "");
        payload.addProperty("game", GameProfile.GAME);
        JsonObject connector = GameProfile.connectorJson();
        if (connector != null) {
            payload.add("connector", connector);
        }
        int mapDimensions = 0;
        if (config.isProtocolV1Enabled()) {
            JsonArray versions = new JsonArray();
            versions.add(0);
            versions.add(1);
            payload.add("protocolVersions", versions);
            List<io.takaro.zomboid.core.model.MapDimension> dims = mapDimensions();
            mapDimensions = dims.size();
            payload.add("capabilities", Capabilities.manifest(dims, gameVersion()));
        }

        JsonObject msg = new JsonObject();
        msg.addProperty("type", "identify");
        msg.add("payload", payload);
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND identify (tokens redacted; protocolVersions="
                    + (config.isProtocolV1Enabled() ? "[0,1], map dimensions: " + mapDimensions : "none") + ")");
        }
        send(msg.toString());
    }

    private void handleIdentifyResponse(JsonObject json) {
        JsonObject payloadObj = optObject(json, "payload");
        JsonObject payload = payloadObj != null ? payloadObj : new JsonObject();

        // Protocol 1 failure shape: a top-level error, after which Takaro closes with 1008.
        JsonObject v1Error = optObject(json, "error");
        if (v1Error != null) {
            TakaroConnector.logBanner(adapter,
                    "Takaro rejected identify: " + optString(v1Error, "code", "?") + " "
                            + optString(v1Error, "message", "") + ".",
                    "Check registrationToken in " + config.getConfigFileHint() + ". Saving a corrected",
                    "token reconnects within a few seconds; set protocol=0 there to fall back to protocol 0.");
            close();
            return;
        }

        if (payload.has("error") && !payload.get("error").isJsonNull()) {
            JsonElement errorElement = payload.get("error");
            // Only name, message and status: the raw error object can embed Takaro's
            // internal request with its auth token.
            String errorMessage = "unknown error";
            String errorName = null;
            int status = 0;
            if (errorElement.isJsonObject()) {
                JsonObject errorObj = errorElement.getAsJsonObject();
                errorMessage = optString(errorObj, "message", errorMessage);
                errorName = optString(errorObj, "name", null);
                status = optInt(errorObj, "http", 0);
            } else if (errorElement.isJsonPrimitive()) {
                errorMessage = errorElement.getAsString();
            }
            String summary = errorMessage + (errorName != null || status != 0
                    ? " (" + (errorName != null ? errorName : "") + (errorName != null && status != 0 ? ", " : "")
                    + (status != 0 ? "http " + status : "") + ")"
                    : "");
            if (status == 409) {
                TakaroConnector.logBanner(adapter,
                        "Takaro rejected identify: " + summary + ".",
                        "Another game server already uses this identity or name, or one with it was",
                        "removed recently. Put a new value after identityToken= in",
                        config.getConfigFileHint() + " and save it; the connector reconnects within a few seconds.");
            } else {
                TakaroConnector.logBanner(adapter,
                        "Takaro rejected identify: " + summary + ".",
                        "Check registrationToken in " + config.getConfigFileHint() + ". Saving a corrected",
                        "token reconnects within a few seconds; until then the connector retries with backoff.");
            }
            // Takaro keeps an unidentified socket open; close it so the backoff retries.
            close();
            return;
        }

        currentReconnectDelay = config.getReconnectDelay();

        // Older Takaro sends no protocolVersion: that is protocol 0.
        protocolVersion = config.isProtocolV1Enabled() ? Math.max(0, optInt(payload, "protocolVersion", 0)) : 0;
        identified = true;

        String serverId = optString(payload, "gameServerId", optString(optObject(payload, "server"), "id", null));
        adapter.logInfo("Identified successfully" + (serverId != null ? ", server ID: " + serverId : "")
                + ", protocol " + protocolVersion);
        flushOutbox();
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
            case "getMapImage":
                if (protocolVersion >= 1) {
                    handleGetMapImage(requestId, args);
                } else {
                    sendError(requestId, ProtocolException.UNSUPPORTED, "Action not implemented: " + action);
                }
                break;
            case "getGameIcon":
                if (protocolVersion >= 1) {
                    handleGetGameIcon(requestId);
                } else {
                    sendError(requestId, ProtocolException.UNSUPPORTED, "Action not implemented: " + action);
                }
                break;
            default:
                sendError(requestId, ProtocolException.UNSUPPORTED, "Action not implemented: " + action);
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
                Throwable cause = error instanceof CompletionException && error.getCause() != null
                        ? error.getCause() : error;
                String message = cause instanceof TimeoutException
                        ? "The game did not answer within 10 seconds" : cause.getMessage();
                adapter.logWarning("Action failed: " + message);
                sendError(requestId, errorCode(cause), message);
            } else if (result != null) {
                sendResponse(requestId, result, null);
            } else if (protocolVersion >= 1) {
                // Protocol 1: nothing to return (or an unknown player) is payload null.
                sendResponse(requestId, JsonNull.INSTANCE, null);
            } else {
                sendResponse(requestId, new JsonObject(), null);
            }
        });
    }

    /** The protocol 1 error code for an action failure. */
    static String errorCode(Throwable error) {
        if (error instanceof ProtocolException pe) return pe.code();
        if (error instanceof UnsupportedOperationException) return ProtocolException.UNSUPPORTED;
        if (error instanceof IllegalArgumentException) return ProtocolException.INVALID_ARGS;
        return ProtocolException.GAME_ERROR;
    }

    private void handleGetMapImage(String requestId, JsonObject args) {
        String dimension = optString(args, "dimension", null);
        double offset = optDouble(args, "offset", -1);
        double length = optDouble(args, "length", -1);
        if (dimension == null || dimension.isEmpty() || offset < 0 || offset != Math.floor(offset)
                || length < 1 || length > MAX_MAP_CHUNK || length != Math.floor(length)) {
            sendError(requestId, ProtocolException.INVALID_ARGS,
                    "getMapImage needs a dimension, an integer offset >= 0 and a length of 1.." + MAX_MAP_CHUNK);
            return;
        }
        try {
            io.takaro.zomboid.core.model.MapImageChunk chunk = adapter.readMapImage(dimension, (long) offset, (int) length);
            JsonObject payload = new JsonObject();
            payload.addProperty("revision", chunk.revision());
            payload.addProperty("data", Base64.getEncoder().encodeToString(chunk.data()));
            sendResponse(requestId, payload, null);
        } catch (ProtocolException e) {
            sendError(requestId, e.code(), e.getMessage());
        } catch (RuntimeException e) {
            adapter.logWarning("getMapImage failed: " + e);
            sendError(requestId, ProtocolException.GAME_ERROR, "Map image read failed: " + e.getMessage());
        }
    }

    /** The whole declared icon in one response; its revision is the one game.profile declares. */
    private void handleGetGameIcon(String requestId) {
        byte[] icon = GameProfile.icon();
        if (icon == null) {
            sendError(requestId, ProtocolException.GAME_ERROR, "The connector has no game icon");
            return;
        }
        JsonObject payload = new JsonObject();
        payload.addProperty("revision", GameProfile.iconRevision());
        payload.addProperty("data", Base64.getEncoder().encodeToString(icon));
        adapter.logInfo("Serving game icon revision " + GameProfile.iconRevision() + " (" + icon.length + " bytes)");
        sendResponse(requestId, payload, null);
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
        // An offline/unknown player has no position. Protocol 1 answers payload null.
        // On protocol 0 a null would go out as an empty {} payload, which Takaro rejects
        // as an invalid IPosition ("x isNumber"), so signal an action error instead.
        if (loc == null) {
            if (protocolVersion >= 1) return null;
            throw new ProtocolException(ProtocolException.NOT_FOUND, "Player not online: " + gameId);
        }
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
            String steamId = ban.steamId();
            if (steamId != null && steamId.matches("\\d{17}")) {
                player.addProperty("steamId", steamId);
                player.addProperty("platformId", PlayerInfo.buildPlatformId(steamId));
            }
            obj.add("player", player);
            obj.addProperty("reason", ban.reason());
            obj.addProperty("expiresAt", ban.expiresAt());
            arr.add(obj);
        }
        return arr;
    }

    // --- JSON helpers ---

    /**
     * The player on an in-game event (chat, death, kill): the same identity fields as getPlayers, so
     * Takaro attributes the event to the same profile, without ip/ping.
     */
    private JsonObject playerIdentityToJson(PlayerInfo p) {
        JsonObject obj = new JsonObject();
        obj.addProperty("gameId", p.gameId());
        obj.addProperty("name", p.name());
        obj.addProperty("steamId", p.steamId());
        obj.addProperty("epicOnlineServicesId", p.epicOnlineServicesId());
        obj.addProperty("xboxLiveId", p.xboxLiveId());
        obj.addProperty("platformId", p.platformId());
        return obj;
    }

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
        sendResponseFrame(requestId, msg);
    }

    /** A failed action: protocol 1 sends {@code error: {code, message}}, protocol 0 the message. */
    private void sendError(String requestId, String code, String message) {
        String text = message != null && !message.isEmpty() ? message : "Game server returned an error";
        if (protocolVersion < 1) {
            sendResponse(requestId, null, text);
            return;
        }
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "response");
        msg.addProperty("requestId", requestId);
        JsonObject error = new JsonObject();
        error.addProperty("code", code);
        error.addProperty("message", text);
        msg.add("error", error);
        sendResponseFrame(requestId, msg);
    }

    private void sendResponseFrame(String requestId, JsonObject msg) {
        String text = msg.toString();
        if (config.isDebugEnabled()) {
            adapter.logDebug("WS SEND response (requestId=" + requestId + "): " + clip(text));
        }
        send(text);
    }

    private static String clip(String text) {
        return text.length() <= MAX_DEBUG_LINE
                ? text : text.substring(0, MAX_DEBUG_LINE) + "... (" + text.length() + " chars)";
    }

    private void scheduleReconnect() {
        adapter.logInfo("Reconnecting in " + (currentReconnectDelay / 1000) + "s...");
        try {
            scheduler.schedule(() -> {
                if (!shouldReconnect || retired) return;
                try {
                    reconnectBlocking();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }, currentReconnectDelay, TimeUnit.MILLISECONDS);
        } catch (RejectedExecutionException e) {
            return; // retired or shut down between the close and this call
        }

        currentReconnectDelay = Math.min(
                (long) (currentReconnectDelay * config.getBackoffMultiplier()),
                config.getMaxReconnectDelay()
        );
    }
}
