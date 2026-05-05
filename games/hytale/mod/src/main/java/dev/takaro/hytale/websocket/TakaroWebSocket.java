package dev.takaro.hytale.websocket;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.config.TakaroConfig;
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
    private final boolean isDev; // true for dev Takaro, false for production
    private boolean isIdentified = false;
    private int reconnectAttempts = 0;
    private static final int MAX_RECONNECT_DELAY = 60000; // 60 seconds
    private static final int BASE_RECONNECT_DELAY = 3000; // 3 seconds
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();

    public TakaroWebSocket(TakaroPlugin plugin, TakaroConfig config, boolean isDev) throws Exception {
        super(new URI(isDev ? config.getDevWsUrl() : config.getWsUrl()));
        this.plugin = plugin;
        this.config = config;
        this.isDev = isDev;
        this.gson = new Gson();
    }

    @Override
    public void onOpen(ServerHandshake handshake) {
        plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Connected to WebSocket");
        reconnectAttempts = 0;
        sendIdentify();
    }

    @Override
    public void onMessage(String message) {
        try {
            JsonObject json = gson.fromJson(message, JsonObject.class);
            String type = json.get("type").getAsString();

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
        plugin.getLogger().at(java.util.logging.Level.WARNING).log(getLogPrefix() + "Disconnected: " + reason);
        isIdentified = false;
        scheduleReconnect();
    }

    @Override
    public void onError(Exception ex) {
        plugin.getLogger().at(java.util.logging.Level.SEVERE).log(getLogPrefix() + "WebSocket error: " + ex.getMessage());
        ex.printStackTrace();
    }

    private String getLogPrefix() {
        return isDev ? "[Dev Takaro] " : "[Takaro] ";
    }

    private void sendIdentify() {
        Map<String, Object> identify = new HashMap<>();
        identify.put("type", "identify");

        Map<String, String> payload = new HashMap<>();
        // Use dev credentials if this is dev connection
        String identityToken = isDev ? config.getDevIdentityToken() : config.getIdentityToken();
        String registrationToken = isDev ? config.getDevRegistrationToken() : config.getRegistrationToken();

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
        if (payload.has("error")) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log(getLogPrefix() + "Identification failed: " + payload.get("error").toString());
        } else {
            plugin.getLogger().at(java.util.logging.Level.INFO).log(getLogPrefix() + "Successfully identified");
            isIdentified = true;
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
        send(gson.toJson(message));
    }

    public void sendResponse(String requestId, Object payload) {
        Map<String, Object> response = new HashMap<>();
        response.put("type", "response");
        response.put("requestId", requestId);
        response.put("payload", payload);
        sendToTakaro(response);
    }

    public void sendGameEvent(String eventType, Map<String, Object> data) {
        if (!isIdentified) {
            return;
        }

        Map<String, Object> event = new HashMap<>();
        event.put("type", "gameEvent");

        Map<String, Object> payload = new HashMap<>();
        payload.put("type", eventType);
        payload.put("data", data);

        event.put("payload", payload);

        // Use FINE level to avoid infinite loop in log forwarding
        plugin.getLogger().at(java.util.logging.Level.FINE).log(getLogPrefix() + "Sending game event: " + eventType);
        sendToTakaro(event);
    }

    private void scheduleReconnect() {
        reconnectAttempts++;
        int exponentialDelay = Math.min(BASE_RECONNECT_DELAY * (int)Math.pow(2, reconnectAttempts - 1), MAX_RECONNECT_DELAY);
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
        scheduler.shutdownNow();
        close();
    }
}
