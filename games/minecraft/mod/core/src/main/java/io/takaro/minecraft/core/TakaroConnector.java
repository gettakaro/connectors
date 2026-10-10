package io.takaro.minecraft.core;

import io.takaro.minecraft.core.config.ConfigFile;
import io.takaro.minecraft.core.target.RuntimeIdentity;
import io.takaro.minecraft.core.target.TargetGuard;
import io.takaro.minecraft.core.target.TargetInfo;

import java.net.URI;
import java.util.Optional;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroConnector {

    static final long CONFIG_POLL_SECONDS = 5;
    private static final String BANNER_RULE =
            "*************************************************************************";

    private final GameAdapter adapter;
    private final TakaroConfig config;
    private final ConfigFile configFile;
    private final Optional<TargetInfo> targetInfo;

    // Guards wsClient, watcher and stopped: start, the config watcher and shutdown run on
    // different threads, and a config change replaces the client while shutdown may be closing it.
    private final Object lock = new Object();
    private TakaroWebSocketClient wsClient;
    private ScheduledExecutorService watcher;
    private boolean stopped;

    public TakaroConnector(GameAdapter adapter, TakaroConfig config) {
        this(adapter, config, TargetInfo.load());
    }

    public TakaroConnector(GameAdapter adapter, TakaroConfig config, Optional<TargetInfo> targetInfo) {
        this(adapter, config, null, targetInfo);
    }

    /** Reads {@code configFile} now and again every few seconds; see {@link #start()}. */
    public TakaroConnector(GameAdapter adapter, ConfigFile configFile) {
        this(adapter, new TakaroConfig(), configFile, TargetInfo.load());
    }

    public TakaroConnector(GameAdapter adapter, ConfigFile configFile, Optional<TargetInfo> targetInfo) {
        this(adapter, new TakaroConfig(), configFile, targetInfo);
    }

    private TakaroConnector(GameAdapter adapter, TakaroConfig config, ConfigFile configFile,
                            Optional<TargetInfo> targetInfo) {
        this.adapter = adapter;
        this.config = config;
        this.configFile = configFile;
        this.targetInfo = targetInfo == null ? Optional.empty() : targetInfo;
    }

    /** Loads the config file, connects when it has a token, and watches the file for changes. */
    public void start() {
        synchronized (lock) {
            if (stopped || configFile == null) {
                return;
            }
            config.copyFrom(configFile.load());
            connectLocked();
            watcher = Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "takaro-config-watch");
                t.setDaemon(true);
                return t;
            });
            watcher.scheduleWithFixedDelay(this::checkConfig,
                    CONFIG_POLL_SECONDS, CONFIG_POLL_SECONDS, TimeUnit.SECONDS);
        }
    }

    public void connect() {
        synchronized (lock) {
            if (!stopped) {
                connectLocked();
            }
        }
    }

    /** One watcher tick: re-read the file and apply what changed. */
    void checkConfig() {
        try {
            TakaroConfig next = configFile.poll();
            if (next != null) {
                applyConfig(next);
            }
        } catch (Throwable t) {
            // An exception would cancel the scheduled task and stop watching for good.
            adapter.logWarning("Checking the Takaro config failed: " + t);
        }
    }

    /**
     * A new URL, token or identity drops the socket and connects at once, skipping any backoff a
     * rejected token built up. Other settings are taken over without touching the connection.
     */
    void applyConfig(TakaroConfig next) {
        synchronized (lock) {
            if (stopped || config.sameSettings(next)) {
                return;
            }
            boolean reconnect = !config.sameConnection(next);
            config.copyFrom(next);
            if (!reconnect) {
                if (wsClient != null) {
                    wsClient.settings().copyFrom(next);
                }
                adapter.logInfo("Reloaded " + configPath() + "; the connection is unchanged");
                return;
            }
            adapter.logInfo(configPath() + " changed; reconnecting with the new settings");
            closeClientLocked();
            connectLocked();
        }
    }

    private void connectLocked() {
        closeClientLocked();

        String url = config.getWsUrl();
        if (url == null || url.isEmpty()) {
            adapter.logWarning("No WebSocket URL configured, cannot connect");
            return;
        }

        // Without a token Takaro can only reject the identify; wait for the watcher instead.
        String token = config.getRegistrationToken();
        if (token == null || token.isEmpty()) {
            logBanner(adapter,
                    "registration_token not set, the server is not connected to Takaro.",
                    "Paste the registration token from Takaro into " + configPath(),
                    "and save it. The connector connects within a few seconds, no restart needed.");
            return;
        }

        String identityToken = config.getIdentityToken();
        if (identityToken == null || identityToken.isEmpty()) {
            logBanner(adapter,
                    "identity_token not set, the server is not connected to Takaro.",
                    "Fix " + configPath() + " so it can be read; the connector",
                    "fills in identity_token itself and connects within a few seconds.");
            return;
        }

        // Check the build target before opening a socket: a mod built for another game
        // version half-works, and half-working looks like a Takaro outage.
        RuntimeIdentity identity = adapter.getRuntimeIdentity();
        TargetGuard.Policy policy = TargetGuard.Policy.parse(config.getTargetPolicy());
        TargetGuard.Decision decision = TargetGuard.evaluate(targetInfo, identity, policy);
        adapter.logInfo("Takaro target-check: "
                + TargetGuard.toJson(targetInfo.orElse(null), identity, decision));
        if (!decision.connect()) {
            String id = targetInfo.map(TargetInfo::target).orElse("unknown");
            adapter.logWarning("Takaro refuses to connect: target " + id
                    + " does not match this server (" + String.join("; ", decision.reasons())
                    + "). Set TAKARO_TARGET_POLICY=warn to override.");
            return;
        }

        adapter.logInfo("Connecting to Takaro at " + url);
        if (config.isDebugEnabled()) {
            adapter.logDebug("Config: wsUrl=" + url + ", reconnect=" + config.isReconnectEnabled()
                    + ", reconnectDelay=" + config.getReconnectDelay() + ", debug=true");
        }
        try {
            // Each client gets its own copy: a config change must never reach a socket that is
            // still identifying with the old settings.
            TakaroConfig settings = new TakaroConfig();
            settings.copyFrom(config);
            wsClient = new TakaroWebSocketClient(new URI(url), adapter, settings, configPath());
            adapter.setEventEmitter(wsClient);
            wsClient.connect();
        } catch (Exception e) {
            wsClient = null;
            adapter.logWarning("Failed to create WebSocket connection: " + e.getMessage());
        }
    }

    private void closeClientLocked() {
        if (wsClient != null) {
            TakaroWebSocketClient old = wsClient;
            wsClient = null;
            adapter.setEventEmitter(null);
            old.shutdown();
        }
    }

    public void shutdown() {
        synchronized (lock) {
            stopped = true;
            if (watcher != null) {
                watcher.shutdownNow();
                watcher = null;
            }
            if (wsClient != null) {
                adapter.logInfo("Shutting down Takaro connection");
                closeClientLocked();
            }
        }
    }

    private String configPath() {
        return configFile != null ? configFile.path().toString() : "the Takaro config file";
    }

    /** Config problems stop the connection outright, so they must stand out in a busy server log. */
    static void logBanner(GameAdapter adapter, String... lines) {
        adapter.logWarning(BANNER_RULE);
        for (String line : lines) {
            adapter.logWarning("  " + line);
        }
        adapter.logWarning(BANNER_RULE);
    }
}
