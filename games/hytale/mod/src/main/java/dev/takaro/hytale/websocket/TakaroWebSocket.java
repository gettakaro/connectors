package dev.takaro.hytale.websocket;

import com.google.gson.Gson;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.config.ConnectionTarget;
import dev.takaro.hytale.config.TakaroConfig;
import dev.takaro.hytale.protocol.Capabilities;
import dev.takaro.hytale.protocol.EventOutbox;
import dev.takaro.hytale.protocol.IdentifyPayload;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroWebSocket extends WebSocketClient {
    private final TakaroPlugin plugin;
    private final TakaroConfig config;
    private final Gson gson;
    private volatile boolean isIdentified = false;
    private int reconnectAttempts = 0;
    private static final int MAX_RECONNECT_DELAY = 60000; // 60 seconds
    private static final int BASE_RECONNECT_DELAY = 3000; // 3 seconds
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "Takaro-Reconnect");
        t.setDaemon(true);
        return t;
    });

    /** Every event until Takaro acknowledges it (protocol 1) or it was sent once (protocol 0). */
    private final EventOutbox outbox;
    private volatile boolean shuttingDown = false;
    private long lastDropLogAt = 0L;
    private long lastDroppedLogged = 0L;
    /** The protocol Takaro chose in the last identifyResponse; 0 until then and after a drop. */
    private volatile int protocol = 0;
    // The map declared in the pending identify, and the one Takaro holds on this connection.
    private String offeredMapJson;
    private String offeredDimension;
    private String offeredMapWorld;
    private volatile String declaredMapJson;
    private volatile String declaredDimension;
    private volatile String declaredMapWorld;
    private static final long MAP_CHECK_SECONDS = 15;
    private static final long DROP_LOG_INTERVAL_MS = 30_000L;

    public TakaroWebSocket(TakaroPlugin plugin, TakaroConfig config) throws Exception {
        super(new URI(config.getConnectionTarget().url()));
        this.plugin = plugin;
        this.config = config;
        this.gson = new Gson();
        this.outbox = new EventOutbox(config.getEventQueueSize());
        scheduler.scheduleWithFixedDelay(this::checkMapDeclaration, MAP_CHECK_SECONDS, MAP_CHECK_SECONDS, TimeUnit.SECONDS);
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
            + "Socket open - identifying (not yet connected to Takaro)");
        synchronized (outbox) {
            isIdentified = false;
            protocol = 0;
        }
        sendIdentify();
    }

    @Override
    public void onMessage(String message) {
        try {
            JsonObject json = gson.fromJson(message, JsonObject.class);
            String type = json.get("type").getAsString();

            WireLogger.frame(config.isDebug(), getLogPrefix(), WireLogger.IN, type,
                json.has("requestId") && !json.get("requestId").isJsonNull()
                    ? json.get("requestId").getAsString() : null,
                json.has("payload") && json.get("payload").isJsonObject()
                    && json.getAsJsonObject("payload").has("action")
                    ? json.getAsJsonObject("payload").get("action").getAsString() : null);

            switch (type) {
                case "identifyResponse":
                    handleIdentifyResponse(json);
                    break;
                case "connected":
                    plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Confirmed connection");
                    break;
                case "request":
                    handleTakaroRequest(json);
                    break;
                case "ping":
                    sendPong();
                    break;
                case "pong":
                    break;
                case "eventAck":
                    handleEventAck(json);
                    break;
                case "eventError":
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                        + "Takaro rejected an event: " + json.get("payload"));
                    break;
                case "capabilitiesResponse":
                    if (json.has("error") && !json.get("error").isJsonNull()) {
                        // Positions must not name a dimension Takaro refused.
                        declaredDimension = null;
                        declaredMapWorld = null;
                        plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                            + "Takaro refused the capability update: " + describeError(json.get("error")));
                    } else {
                        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
                            + "Takaro accepted the capability update");
                    }
                    break;
                case "error":
                    handleError(json);
                    break;
                default:
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log("Unknown message type from Takaro: " + type);
            }
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling message: " + e.getMessage());
            e.printStackTrace();
        }
    }

    @Override
    public void onClose(int code, String reason, boolean remote) {
        boolean wasIdentified = isIdentified;
        synchronized (outbox) {
            isIdentified = false;
            protocol = 0;
            declaredDimension = null;
            declaredMapWorld = null;
            declaredMapJson = null;
        }
        if (shuttingDown) {
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Connection closed (shutting down)");
            return;
        }
        plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
            + "Disconnected (code " + code + ", remote=" + remote + "): " + reason
            + (wasIdentified ? " - Takaro now considers this server offline" : "")
            + " | " + outbox.size() + " event(s) waiting for Takaro");
        scheduleReconnect();
    }

    @Override
    public void onError(Exception ex) {
        plugin.getLogger().at(java.util.logging.Level.SEVERE).log(getLogPrefix() + "WebSocket error: " + ex.getMessage());
        ex.printStackTrace();
    }

    private String getLogPrefix() {
        return "[Takaro] ";
    }

    private void sendIdentify() {
        // Re-read the properties file first, so an operator who fixes a stale token does not
        // have to restart the whole server.
        config.reload();

        Map<String, Object> payload = new LinkedHashMap<>();
        ConnectionTarget target = config.getConnectionTarget();
        String identityToken = target.identityToken();
        String registrationToken = target.registrationToken();

        payload.put("identityToken", identityToken);
        if (!registrationToken.isEmpty()) {
            payload.put("registrationToken", registrationToken);
        }

        Map<String, Object> mapDeclaration = currentMapDeclaration();
        offeredMapJson = mapDeclaration == null ? null : gson.toJson(mapDeclaration);
        offeredDimension = mapDeclaration == null ? null : String.valueOf(mapDeclaration.get("id"));
        offeredMapWorld = mapDeclaration == null ? null : plugin.getWorldMap().worldName();
        payload.putAll(IdentifyPayload.build(plugin.getVersion(), plugin.getServerVersion(), mapDeclaration));

        Map<String, Object> identify = new LinkedHashMap<>();
        identify.put("type", "identify");
        identify.put("payload", payload);

        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
            + "Sending identify (protocol versions 0 and 1"
            + (mapDeclaration != null ? ", map dimension '" + offeredDimension + "'" : ", no map yet") + ")");
        send(gson.toJson(identify));
    }

    private void handleIdentifyResponse(JsonObject message) {
        JsonObject payload = message.has("payload") && message.get("payload").isJsonObject()
            ? message.getAsJsonObject("payload") : new JsonObject();
        // Protocol 0 puts the error in the payload; protocol 1 sends a top-level error object.
        JsonElement errorElement = payload.has("error") && !payload.get("error").isJsonNull() ? payload.get("error")
            : message.has("error") && !message.get("error").isJsonNull() ? message.get("error") : null;
        if (errorElement != null) {
            isIdentified = false;
            String error = describeError(errorElement);
            // An identification failure used to be logged once and then silently end all
            // reconnect attempts, leaving a server that looked healthy on the console while
            // Takaro saw it as offline. It is retryable: say why, loudly, and keep
            // trying with backoff.
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                + "Identification REJECTED by Takaro: " + error
                + " | this server is NOT connected to Takaro."
                + " Check IDENTITY_TOKEN and REGISTRATION_TOKEN in TakaroConfig.properties;"
                + " the file is re-read before every attempt, so a fix takes effect without a restart."
                + " Retrying with backoff (attempt " + (reconnectAttempts + 1) + ").");
            close();
            return;
        }

        // Switching protocol and resending the backlog under the outbox lock keeps a new event
        // from leaving ahead of older unacknowledged ones.
        synchronized (outbox) {
            protocol = IdentifyPayload.chosenProtocol(payload);
            isIdentified = true;
            reconnectAttempts = 0;
            if (protocol >= 1) {
                declaredMapJson = offeredMapJson;
                declaredDimension = offeredDimension;
                declaredMapWorld = offeredMapWorld;
            }
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
                + "Successfully identified (protocol v" + protocol + ")");
            if (protocol >= 1) {
                List<EventOutbox.Pending> backlog = outbox.unacknowledged();
                if (!backlog.isEmpty()) {
                    plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
                        + "Resending " + backlog.size() + " event(s) Takaro has not acknowledged"
                        + (outbox.dropped() > 0 ? " (" + outbox.dropped() + " dropped while full)" : ""));
                }
                for (EventOutbox.Pending event : backlog) {
                    sendEventFrame(event.payload());
                }
            } else {
                List<EventOutbox.Pending> backlog = outbox.drain();
                if (!backlog.isEmpty()) {
                    plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
                        + "Flushing " + backlog.size() + " queued game event(s)"
                        + (outbox.dropped() > 0 ? " (" + outbox.dropped() + " were dropped while full)" : ""));
                }
                for (EventOutbox.Pending event : backlog) {
                    sendEventFrame(EventOutbox.toV0Payload(event.payload()));
                }
            }
        }
    }

    /** Only code, name and message: a full identify error can echo Takaro's request headers. */
    private static String describeError(JsonElement error) {
        if (error.isJsonObject()) {
            JsonObject e = error.getAsJsonObject();
            StringBuilder sb = new StringBuilder();
            for (String key : new String[]{"code", "name", "message"}) {
                if (e.has(key) && e.get(key).isJsonPrimitive()) {
                    if (sb.length() > 0) {
                        sb.append(": ");
                    }
                    sb.append(e.get(key).getAsString());
                }
            }
            return sb.length() == 0 ? "unknown error" : sb.toString();
        }
        return error.isJsonPrimitive() ? error.getAsString() : error.toString();
    }

    private void handleEventAck(JsonObject message) {
        JsonObject payload = message.has("payload") && message.get("payload").isJsonObject()
            ? message.getAsJsonObject("payload") : null;
        if (payload == null || !payload.has("streamId")) {
            return;
        }
        JsonElement upTo = payload.has("upTo") ? payload.get("upTo") : payload.get("seq");
        if (upTo == null || !upTo.isJsonPrimitive()) {
            return;
        }
        try {
            outbox.acknowledge(payload.get("streamId").getAsString(), upTo.getAsLong());
        } catch (RuntimeException e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Unreadable eventAck: " + payload);
        }
    }

    private void handleTakaroRequest(JsonObject message) {
        String requestId = message.get("requestId").getAsString();
        JsonObject payload = message.getAsJsonObject("payload");
        String action = payload.get("action").getAsString();

        plugin.getLogger().at(java.util.logging.Level.FINE).log(getLogPrefix() + "Received Takaro request: " + action);

        // Delegate to plugin's request handler, passing this WebSocket for response
        plugin.handleTakaroRequest(this, requestId, action, payload);
    }

    private void handleError(JsonObject message) {
        plugin.getLogger().at(java.util.logging.Level.SEVERE).log(getLogPrefix() + "Error: " + message.toString());

        // Check if this is the "Internal error handling game event" error
        if (message.has("payload")) {
            JsonObject payload = message.getAsJsonObject("payload");
            if (payload.has("message")) {
                String errorMessage = payload.get("message").getAsString();
                if ("Internal error handling game event".equals(errorMessage)) {
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Detected internal error - reconnecting with fresh connection");
                    // Close and reconnect with fresh websocket connection
                    // This will trigger onClose() which will call scheduleReconnect()
                    // The reconnect will send fresh identify with ID and registration tokens
                    close();
                    return;
                }
            }
        }

        // For other errors, just log and stay connected
    }

    private void sendPong() {
        Map<String, String> pong = new HashMap<>();
        pong.put("type", "pong");
        send(gson.toJson(pong));
    }

    public void sendToTakaro(Map<String, Object> message) {
        if (!isOpen()) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Cannot send - not connected");
            return;
        }
        try {
            WireLogger.frame(config.isDebug(), getLogPrefix(), WireLogger.OUT,
                String.valueOf(message.get("type")),
                message.get("requestId") == null ? null : String.valueOf(message.get("requestId")),
                eventTypeOf(message),
                config.isDebug() ? dev.takaro.hytale.util.IdentityTrace.of(message) : null);
            send(gson.toJson(message));
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Send failed: " + e.getMessage());
        }
    }

    public void sendResponse(String requestId, Object payload) {
        Map<String, Object> response = new HashMap<>();
        response.put("type", "response");
        response.put("requestId", requestId);
        response.put("payload", payload);
        sendToTakaro(response);
    }

    /** A protocol 1 response; a null payload is sent as an explicit {@code payload: null}. */
    public void sendV1Response(String requestId, Object payload) {
        JsonObject frame = new JsonObject();
        frame.addProperty("type", "response");
        frame.addProperty("requestId", requestId);
        frame.add("payload", payload == null ? JsonNull.INSTANCE : gson.toJsonTree(payload));
        sendFrame(frame, "response", requestId, null);
    }

    /**
     * A failed request: on protocol 1 the error envelope {@code {error: {code, message}}}, on
     * protocol 0 the old {@code payload: {error: message}}.
     */
    public void sendError(String requestId, String code, String message) {
        String text = message == null || message.isBlank() ? code : message;
        if (!isV1()) {
            Map<String, Object> error = new HashMap<>();
            error.put("error", text);
            sendResponse(requestId, error);
            return;
        }
        JsonObject frame = new JsonObject();
        frame.addProperty("type", "response");
        frame.addProperty("requestId", requestId);
        JsonObject error = new JsonObject();
        error.addProperty("code", code);
        error.addProperty("message", text);
        frame.add("error", error);
        sendFrame(frame, "response", requestId, null);
    }

    private void sendFrame(JsonObject frame, String type, String requestId, String action) {
        if (!isOpen()) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Cannot send - not connected");
            return;
        }
        try {
            WireLogger.frame(config.isDebug(), getLogPrefix(), WireLogger.OUT, type, requestId, action);
            send(frame.toString());
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Send failed: " + e.getMessage());
        }
    }

    public void sendGameEvent(String eventType, Map<String, Object> data) {
        if (shuttingDown) {
            return;
        }
        // Events come from several threads, and Takaro ignores a seq at or below one it has
        // already seen, so numbering and sending share one lock and leave in seq order.
        synchronized (outbox) {
            EventOutbox.Pending event = outbox.add(eventType, data);
            logDropsIfAny();
            // Nothing goes out before identifyResponse; the outbox keeps it until then.
            if (!isIdentified || !isOpen()) {
                return;
            }
            if (isV1()) {
                sendEventFrame(event.payload());
            } else {
                // Protocol 0 has no acks: send it once, in the old shape.
                outbox.acknowledge(outbox.streamId(), event.seq());
                sendEventFrame(EventOutbox.toV0Payload(event.payload()));
            }
        }
    }

    private void sendEventFrame(Map<String, Object> payload) {
        Map<String, Object> event = new HashMap<>();
        event.put("type", "gameEvent");
        event.put("payload", payload);
        sendToTakaro(event);
    }

    private void logDropsIfAny() {
        long dropped = outbox.dropped();
        if (dropped == lastDroppedLogged) {
            return;
        }
        long now = System.currentTimeMillis();
        if (now - lastDropLogAt > DROP_LOG_INTERVAL_MS) {
            lastDropLogAt = now;
            lastDroppedLogged = dropped;
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                + "Event buffer full (" + outbox.capacity() + "), dropping oldest events - "
                + dropped + " dropped in total since startup");
        }
    }

    /** The map Dimension to declare now, or null while the map is off or not ready. */
    private Map<String, Object> currentMapDeclaration() {
        try {
            dev.takaro.hytale.map.WorldMapService map = plugin.getWorldMap();
            return map == null ? null : map.declaration();
        } catch (RuntimeException e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                + "Could not build the map declaration: " + e);
            return null;
        }
    }

    /**
     * Declares the map once it is ready, and again when its frame grew, with a full
     * {@code capabilities} replacement: Takaro only takes the whole manifest.
     */
    void checkMapDeclaration() {
        try {
            if (!isV1() || !isIdentified || !isOpen() || shuttingDown) {
                return;
            }
            Map<String, Object> declaration = currentMapDeclaration();
            String json = declaration == null ? null : gson.toJson(declaration);
            if (java.util.Objects.equals(json, declaredMapJson)) {
                return;
            }
            Map<String, Object> payload = new LinkedHashMap<>();
            payload.put("capabilities", Capabilities.manifest(plugin.getServerVersion(), declaration));
            Map<String, Object> frame = new LinkedHashMap<>();
            frame.put("type", "capabilities");
            frame.put("requestId", java.util.UUID.randomUUID().toString());
            frame.put("payload", payload);
            declaredMapJson = json;
            declaredDimension = declaration == null ? null : String.valueOf(declaration.get("id"));
            declaredMapWorld = declaration == null ? null : plugin.getWorldMap().worldName();
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
                + "Sending capability update (map dimension " + (declaredDimension == null ? "none" : "'" + declaredDimension + "'") + ")");
            sendToTakaro(frame);
        } catch (RuntimeException e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Map declaration check failed: " + e);
        }
    }

    /** True once Takaro chose protocol 1 on the current connection. */
    public boolean isV1() {
        return protocol >= 1;
    }

    public int protocolVersion() {
        return protocol;
    }

    /**
     * The declared map Dimension id for a position in {@code worldName}, or null: on protocol 0,
     * before the map is declared, and for any other world than the one the map draws.
     */
    public String dimensionFor(String worldName) {
        String world = declaredMapWorld;
        String dimension = declaredDimension;
        if (!isV1() || world == null || dimension == null || worldName == null) {
            return null;
        }
        return world.equals(worldName) ? dimension : null;
    }

    @SuppressWarnings("unchecked")
    private static String eventTypeOf(Map<String, Object> message) {
        Object payload = message.get("payload");
        if (payload instanceof Map) {
            Object type = ((Map<String, Object>) payload).get("type");
            return type == null ? null : String.valueOf(type);
        }
        return null;
    }

    /** Number of events not yet acknowledged by Takaro (for /takarodebug). */
    public int getPendingEventCount() {
        return outbox.size();
    }

    /** Total events discarded because the buffer was full. */
    public long getDroppedEventCount() {
        return outbox.dropped();
    }

    /**
     * Backoff before reconnect attempt {@code attempt} (1-based): 3 s doubling up to 60 s. The
     * exponent is capped so a long outage cannot overflow into a negative or multi-week delay.
     */
    static int reconnectDelayMs(int attempt) {
        int exponent = Math.max(0, Math.min(attempt - 1, 16));
        long delay = (long) BASE_RECONNECT_DELAY << exponent;
        return (int) Math.min(delay, MAX_RECONNECT_DELAY);
    }

    private void scheduleReconnect() {
        reconnectAttempts++;
        int exponentialDelay = reconnectDelayMs(reconnectAttempts);
        int jitter = (int)(Math.random() * exponentialDelay * 0.25);
        int delayMs = exponentialDelay + jitter;

        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Scheduling reconnect attempt " + reconnectAttempts + " in " + (delayMs / 1000) + "s");

        scheduler.schedule(() -> {
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Attempting to reconnect...");
            try {
                reconnect();
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log(getLogPrefix() + "Reconnect failed: " + e.getMessage());
                e.printStackTrace();
                // Schedule another reconnect attempt
                scheduleReconnect();
            }
        }, delayMs, TimeUnit.MILLISECONDS);
    }

    public boolean isIdentified() {
        return isIdentified;
    }

    public void shutdown() {
        shuttingDown = true;
        scheduler.shutdownNow();
        outbox.clear();
        close();
    }
}
