package io.takaro.zomboid.core;

import java.net.URI;

/**
 * Owns the one live Takaro socket. The first server tick starts it, a config change
 * replaces it ({@link #applyConfig}) and JVM shutdown stops it; all three are serialised
 * on one lock, so a reload never races a shutdown or leaves two sockets open.
 */
public class TakaroConnector {

    private static final String RULE =
            "*************************************************************************";

    private final GameAdapter adapter;
    private final TakaroConfig config;
    private final Object lock = new Object();
    private TakaroWebSocketClient wsClient;
    private boolean started;
    private boolean stopped;

    public TakaroConnector(GameAdapter adapter, TakaroConfig config) {
        this.adapter = adapter;
        this.config = config;
    }

    public void connect() {
        synchronized (lock) {
            if (stopped) {
                return;
            }
            started = true;
            openLocked();
        }
    }

    /**
     * Takes over a reloaded config. A changed URL or token drops the current socket and
     * connects at once, skipping whatever backoff a rejected token had built up; any other
     * change only updates the shared settings.
     */
    public void applyConfig(TakaroConfig updated) {
        synchronized (lock) {
            boolean reconnect = !config.sameConnection(updated);
            config.copyFrom(updated);
            if (!reconnect || !started || stopped) {
                return;
            }
            adapter.logInfo("Config changed; reconnecting to Takaro with the new settings");
            openLocked();
        }
    }

    public void shutdown() {
        TakaroWebSocketClient client;
        synchronized (lock) {
            stopped = true;
            client = wsClient;
            wsClient = null;
        }
        if (client != null) {
            adapter.logInfo("Shutting down Takaro connection");
            client.shutdown();
        }
    }

    /** A loud multi-line block, so a config problem stands out in a busy server console. */
    public static void logBanner(GameAdapter adapter, String... lines) {
        adapter.logWarning(RULE);
        for (String line : lines) {
            adapter.logWarning("  " + line);
        }
        adapter.logWarning(RULE);
    }

    private void openLocked() {
        if (wsClient != null) {
            wsClient.retire();
            wsClient = null;
        }

        String url = config.getWsUrl();
        if (url == null || url.isEmpty()) {
            adapter.logWarning("No WebSocket URL configured, cannot connect");
            return;
        }

        // Without a token Takaro never identifies the socket; wait for the config watcher.
        if (!TakaroConfig.isSet(config.getRegistrationToken())) {
            logBanner(adapter,
                    "registrationToken not set, the server is not connected to Takaro.",
                    "Paste the registration token from Takaro into " + config.getConfigFileHint(),
                    "and save it. The connector connects within a few seconds, no restart needed.");
            return;
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
            wsClient = null;
            adapter.logWarning("Failed to create WebSocket connection: " + e.getMessage());
        }
    }
}
