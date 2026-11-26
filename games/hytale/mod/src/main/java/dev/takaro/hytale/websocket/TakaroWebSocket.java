package dev.takaro.hytale.websocket;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.config.ConnectionTarget;
import dev.takaro.hytale.config.TakaroConfig;
import dev.takaro.hytale.state.BoundedEventQueue;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

import java.net.URI;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroWebSocket extends WebSocketClient {
    private final TakaroPlugin plugin;
    private final TakaroConfig config;
    private final Gson gson;
    private boolean isIdentified = false;
    private int reconnectAttempts = 0;
    private static final int MAX_RECONNECT_DELAY = 60000; // 60 seconds
    private static final int BASE_RECONNECT_DELAY = 3000; // 3 seconds
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "Takaro-Reconnect");
        t.setDaemon(true);
        return t;
    });

    /** Events produced while the connection is down or not yet identified. */
    private final BoundedEventQueue<Map<String, Object>> pendingEvents;
    private volatile boolean shuttingDown = false;
    private long lastDropLogAt = 0L;
    private static final long DROP_LOG_INTERVAL_MS = 30_000L;

    public TakaroWebSocket(TakaroPlugin plugin, TakaroConfig config) throws Exception {
        super(new URI(config.getConnectionTarget().url()));
        this.plugin = plugin;
        this.config = config;
        this.gson = new Gson();
        this.pendingEvents = new BoundedEventQueue<>(config.getEventQueueSize());
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
            + "Socket open - identifying (not yet connected to Takaro)");
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
        isIdentified = false;
        if (shuttingDown) {
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Connection closed (shutting down)");
            return;
        }
        plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
            + "Disconnected (code " + code + ", remote=" + remote + "): " + reason
            + (wasIdentified ? " - Takaro now considers this server offline" : "")
            + " | " + pendingEvents.size() + " event(s) queued");
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

        Map<String, Object> identify = new HashMap<>();
        identify.put("type", "identify");

        Map<String, String> payload = new HashMap<>();
        ConnectionTarget target = config.getConnectionTarget();
        String identityToken = target.identityToken();
        String registrationToken = target.registrationToken();

        payload.put("identityToken", identityToken);
        if (!registrationToken.isEmpty()) {
            payload.put("registrationToken", registrationToken);
        }

        identify.put("payload", payload);

        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Sending identify message");
        send(gson.toJson(identify));
    }

    private void handleIdentifyResponse(JsonObject message) {
        JsonObject payload = message.getAsJsonObject("payload");
        if (payload != null && payload.has("error") && !payload.get("error").isJsonNull()) {
            isIdentified = false;
            String error = payload.get("error").toString();
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

        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Successfully identified");
        isIdentified = true;
        reconnectAttempts = 0;
        flushPendingEvents();
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

    public void sendGameEvent(String eventType, Map<String, Object> data) {
        Map<String, Object> event = new HashMap<>();
        event.put("type", "gameEvent");

        Map<String, Object> payload = new HashMap<>();
        payload.put("type", eventType);
        payload.put("data", data);

        event.put("payload", payload);

        // Takaro must not receive a gameEvent before identifyResponse, and events produced
        // while the socket is down must not be lost. Park them instead of dropping them.
        if (!isIdentified || !isOpen()) {
            if (shuttingDown) {
                return;
            }
            boolean fitted = pendingEvents.offer(event);
            if (!fitted) {
                long now = System.currentTimeMillis();
                if (now - lastDropLogAt > DROP_LOG_INTERVAL_MS) {
                    lastDropLogAt = now;
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix()
                        + "Event queue full (" + pendingEvents.capacity() + "), dropping oldest events - "
                        + pendingEvents.droppedCount() + " dropped in total since startup");
                }
            }
            return;
        }

        sendToTakaro(event);
    }

    /** Send everything that was parked while the connection was unusable. */
    private void flushPendingEvents() {
        if (pendingEvents.isEmpty()) {
            return;
        }
        java.util.List<Map<String, Object>> queued = pendingEvents.drain();
        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix()
            + "Flushing " + queued.size() + " queued game event(s)"
            + (pendingEvents.droppedCount() > 0 ? " (" + pendingEvents.droppedCount() + " were dropped while full)" : ""));
        for (Map<String, Object> event : queued) {
            if (!isOpen()) {
                // Connection died mid-flush; keep what is left rather than losing it.
                pendingEvents.offer(event);
                continue;
            }
            sendToTakaro(event);
        }
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

    /** Number of events currently waiting to be sent (for /takarodebug). */
    public int getPendingEventCount() {
        return pendingEvents.size();
    }

    /** Total events discarded because the queue was full. */
    public long getDroppedEventCount() {
        return pendingEvents.droppedCount();
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
        pendingEvents.clear();
        close();
    }
}
