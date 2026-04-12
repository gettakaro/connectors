package io.takaro.minecraft.core;

import java.net.URI;

public class TakaroConnector {

    private final GameAdapter adapter;
    private final TakaroConfig config;
    private TakaroWebSocketClient wsClient;

    public TakaroConnector(GameAdapter adapter, TakaroConfig config) {
        this.adapter = adapter;
        this.config = config;
    }

    public void connect() {
        String url = config.getWsUrl();
        if (url == null || url.isEmpty()) {
            adapter.logWarning("No WebSocket URL configured, cannot connect");
            return;
        }

        if (wsClient != null) {
            wsClient.shutdown();
        }

        adapter.logInfo("Connecting to Takaro at " + url);
        if (config.isDebugEnabled()) {
            adapter.logDebug("Config: wsUrl=" + url + ", reconnect=" + config.isReconnectEnabled()
                    + ", reconnectDelay=" + config.getReconnectDelay() + ", debug=true");
        }
        try {
            wsClient = new TakaroWebSocketClient(new URI(url), adapter, config);
            adapter.setEventEmitter(wsClient);
            wsClient.connect();
        } catch (Exception e) {
            adapter.logWarning("Failed to create WebSocket connection: " + e.getMessage());
        }
    }

    public void shutdown() {
        if (wsClient != null) {
            adapter.logInfo("Shutting down Takaro connection");
            wsClient.shutdown();
        }
    }
}
