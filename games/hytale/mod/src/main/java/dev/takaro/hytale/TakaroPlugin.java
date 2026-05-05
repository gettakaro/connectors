package dev.takaro.hytale;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.hypixel.hytale.server.core.HytaleServer;
import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.plugin.JavaPlugin;
import com.hypixel.hytale.server.core.plugin.JavaPluginInit;
import com.hypixel.hytale.server.core.universe.PlayerRef;
import com.hypixel.hytale.server.core.universe.Universe;
import com.hypixel.hytale.logger.backend.HytaleLoggerBackend;
import dev.takaro.hytale.api.HytaleApiClient;
import dev.takaro.hytale.commands.TakaroDebugCommand;
import dev.takaro.hytale.config.TakaroConfig;
import dev.takaro.hytale.events.ChatEventListener;
import dev.takaro.hytale.events.PlayerDeathSystem;
import dev.takaro.hytale.events.PlayerEventListener;
import dev.takaro.hytale.events.TakaroLogHandler;
import dev.takaro.hytale.handlers.TakaroRequestHandler;
import dev.takaro.hytale.websocket.TakaroWebSocket;

import javax.annotation.Nonnull;
import java.awt.Color;
import java.io.File;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroPlugin extends JavaPlugin {
    private static final String VERSION = "1.14.6";
    private static final String HYTALECHARTS_API_URL = "https://hytalecharts.com/api/heartbeat";
    private static final int HEARTBEAT_INTERVAL_SECONDS = 300; // 5 minutes

    private TakaroConfig config;
    private TakaroWebSocket webSocket;
    private TakaroWebSocket devWebSocket; // Optional dev Takaro connection
    private TakaroRequestHandler requestHandler;
    private HytaleApiClient hytaleApi; // Hidden feature - not in user config yet
    private ChatEventListener chatListener;
    private PlayerEventListener playerListener;
    private PlayerDeathSystem deathSystem;
    private TakaroLogHandler logHandler;
    private ScheduledExecutorService telemetryScheduler;

    // HytaleCharts integration
    private HttpClient httpClient;
    private ScheduledExecutorService hytaleChartsScheduler;
    private volatile int trackedPlayerCount = 0;

    // Cache for player name colors (UUID -> color code)
    // Set by Takaro via setPlayerNameColor action
    private final ConcurrentHashMap<String, String> playerNameColors = new ConcurrentHashMap<>();

    public TakaroPlugin(@Nonnull JavaPluginInit init) {
        super(init);
    }

    @Override
    protected void setup() {
        getLogger().at(java.util.logging.Level.INFO).log("Hytale-Takaro Integration v" + VERSION + " initializing...");

        // Load configuration - store in HytaleTakaroMod subfolder
        File configFile = getFile().getParent().resolve("HytaleTakaroMod").resolve("TakaroConfig.properties").toFile();
        config = new TakaroConfig(configFile);

        // Initialize Hytale API client (hidden feature - optional)
        hytaleApi = new HytaleApiClient(this, config.getHytaleApiUrl());
        if (!config.getHytaleApiToken().isEmpty()) {
            hytaleApi.setAuthToken(config.getHytaleApiToken());
            getLogger().at(java.util.logging.Level.INFO).log("Hytale API client initialized");
        }

        // Initialize HytaleCharts HTTP client
        httpClient = HttpClient.newBuilder()
            .connectTimeout(Duration.ofSeconds(10))
            .build();

        // Initialize request handler
        requestHandler = new TakaroRequestHandler(this, hytaleApi);

        // Initialize event listeners
        chatListener = new ChatEventListener(this);
        playerListener = new PlayerEventListener(this);
        deathSystem = new PlayerDeathSystem(this);
        logHandler = new TakaroLogHandler(this);

        // Register events (official pattern)
        registerEvents();

        // Register ECS systems
        registerEcsSystems();

        // Subscribe to Hytale's logging system
        HytaleLoggerBackend.subscribe(logHandler.getLogBuffer());

        // Register debug command (official pattern)
        HytaleServer.get().getCommandManager().register(new TakaroDebugCommand(this));

        getLogger().at(java.util.logging.Level.INFO).log("Configuration loaded");
        getLogger().at(java.util.logging.Level.INFO).log("Debug command registered: /takarodebug");
    }

    /**
     * Register event handlers using official Hytale pattern
     */
    private void registerEvents() {
        try {
            // Register chat event
            this.getEventRegistry().registerGlobal(
                com.hypixel.hytale.server.core.event.events.player.PlayerChatEvent.class,
                chatListener::onPlayerChat
            );
            getLogger().at(java.util.logging.Level.INFO).log("Registered PlayerChatEvent handler");

            // Register player connect event
            this.getEventRegistry().registerGlobal(
                com.hypixel.hytale.server.core.event.events.player.PlayerConnectEvent.class,
                playerListener::onPlayerConnect
            );
            getLogger().at(java.util.logging.Level.INFO).log("Registered PlayerConnectEvent handler");

            // Register player disconnect event
            this.getEventRegistry().registerGlobal(
                com.hypixel.hytale.server.core.event.events.player.PlayerDisconnectEvent.class,
                playerListener::onPlayerDisconnect
            );
            getLogger().at(java.util.logging.Level.INFO).log("Registered PlayerDisconnectEvent handler");

        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.SEVERE).log("Failed to register events: " + e.getMessage());
            e.printStackTrace();
        }
    }

    /**
     * Register ECS systems using official Hytale pattern
     */
    private void registerEcsSystems() {
        try {
            // Register player death system with entity store registry
            this.getEntityStoreRegistry().registerSystem(deathSystem);
            getLogger().at(java.util.logging.Level.INFO).log("Registered PlayerDeathSystem");

        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.SEVERE).log("Failed to register ECS systems: " + e.getMessage());
            e.printStackTrace();
        }
    }

    @Override
    protected void start() {
        super.start();
        getLogger().at(java.util.logging.Level.INFO).log("Starting Takaro WebSocket connection...");

        // Check if items are loaded
        try {
            com.hypixel.hytale.server.core.asset.type.item.config.Item.getAssetMap();
            int itemCount = com.hypixel.hytale.server.core.asset.type.item.config.Item.getAssetMap().getAssetMap().size();
            getLogger().at(java.util.logging.Level.INFO).log("Items loaded: " + itemCount + " items available");
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.WARNING).log("Could not check items at startup: " + e.getMessage());
        }

        // Connect to production Takaro
        try {
            webSocket = new TakaroWebSocket(this, config, false); // false = production
            webSocket.connect();
            getLogger().at(java.util.logging.Level.INFO).log("Connecting to Takaro at " + config.getWsUrl());
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.SEVERE).log("Failed to start WebSocket connection: " + e.getMessage());
            e.printStackTrace();
        }

        // Connect to dev Takaro (if enabled)
        if (config.isDevEnabled()) {
            try {
                devWebSocket = new TakaroWebSocket(this, config, true); // true = dev
                devWebSocket.connect();
                getLogger().at(java.util.logging.Level.INFO).log("Connecting to Dev Takaro at " + config.getDevWsUrl());
            } catch (Exception e) {
                getLogger().at(java.util.logging.Level.SEVERE).log("Failed to start Dev WebSocket connection: " + e.getMessage());
                e.printStackTrace();
            }
        }

        // Start telemetry reporting to Hytale API (optional - only if token configured)
        if (!config.getHytaleApiToken().isEmpty()) {
            telemetryScheduler = Executors.newSingleThreadScheduledExecutor();
            telemetryScheduler.scheduleAtFixedRate(this::reportTelemetry, 1, 5, TimeUnit.MINUTES);
            getLogger().at(java.util.logging.Level.INFO).log("Started Hytale telemetry reporting");
        }

        // Start HytaleCharts integration (if configured)
        startHytaleCharts();

        // Start log forwarding to Takaro
        logHandler.start();
    }

    private void reportTelemetry() {
        try {
            int playerCount = getReliablePlayerCount();

            JsonObject metadata = new JsonObject();
            metadata.addProperty("takaroEnabled", true);
            metadata.addProperty("version", VERSION);

            hytaleApi.reportTelemetry(playerCount, "online", metadata);
            getLogger().at(java.util.logging.Level.FINE).log("Telemetry reported to Hytale API");
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.WARNING).log("Failed to report telemetry: " + e.getMessage());
        }
    }

    /**
     * Start HytaleCharts integration - heartbeat and promo links
     */
    private void startHytaleCharts() {
        String secret = config.getHytaleChartsSecret();
        if (secret == null || secret.isEmpty() || secret.equals("YOUR_SECRET_HERE")) {
            getLogger().at(java.util.logging.Level.WARNING).log("HytaleCharts not configured! Generate a heartbeat secret at hytalecharts.com");
            return;
        }

        hytaleChartsScheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "HytaleCharts-Heartbeat");
            t.setDaemon(true);
            return t;
        });

        hytaleChartsScheduler.scheduleAtFixedRate(this::sendHeartbeat, 5, HEARTBEAT_INTERVAL_SECONDS, TimeUnit.SECONDS);

        // Schedule promo link broadcast if enabled
        if (config.getHytaleChartsPromoEnabled() && config.getHytaleChartsPromoIntervalMinutes() > 0) {
            long intervalSeconds = config.getHytaleChartsPromoIntervalMinutes() * 60L;
            hytaleChartsScheduler.scheduleAtFixedRate(this::broadcastPromoLink, intervalSeconds, intervalSeconds, TimeUnit.SECONDS);
            if (config.getHytaleChartsDebug()) {
                getLogger().at(java.util.logging.Level.INFO).log("Promo link broadcasts enabled - every %d minutes", config.getHytaleChartsPromoIntervalMinutes());
            }
        }

        // Register player count tracking events for HytaleCharts
        this.getEventRegistry().registerGlobal(
            com.hypixel.hytale.server.core.event.events.player.PlayerConnectEvent.class,
            event -> {
                trackedPlayerCount++;
                PlayerRef playerRef = event.getPlayerRef();
                if (config.getHytaleChartsDebug()) {
                    getLogger().at(java.util.logging.Level.INFO).log("Player connected: %s (tracked: %d)",
                        playerRef.getUsername(), trackedPlayerCount);
                }
                // Send promo link on login if enabled (with delay for player to load)
                if (config.getHytaleChartsPromoOnLogin()) {
                    hytaleChartsScheduler.schedule(() -> sendPromoLinkToPlayer(playerRef), 22, TimeUnit.SECONDS);
                }
            }
        );

        this.getEventRegistry().registerGlobal(
            com.hypixel.hytale.server.core.event.events.player.PlayerDisconnectEvent.class,
            event -> {
                trackedPlayerCount = Math.max(0, trackedPlayerCount - 1);
                if (config.getHytaleChartsDebug()) {
                    getLogger().at(java.util.logging.Level.INFO).log("Player disconnected: %s (tracked: %d)",
                        event.getPlayerRef().getUsername(), trackedPlayerCount);
                }
            }
        );

        getLogger().at(java.util.logging.Level.INFO).log("HytaleCharts integration started - sending heartbeat every %d seconds", HEARTBEAT_INTERVAL_SECONDS);
    }

    /**
     * Gets the most accurate player count by comparing event-based tracking
     * with the authoritative Universe.getPlayerCount(). Uses the higher value
     * to avoid undercounting when events are missed on high-traffic servers.
     * Also syncs the tracked count if drift is detected.
     */
    private int getReliablePlayerCount() {
        int universeCount = Universe.get().getPlayerCount();
        int tracked = trackedPlayerCount;

        if (universeCount != tracked) {
            if (config.getHytaleChartsDebug()) {
                getLogger().at(java.util.logging.Level.INFO).log("Player count drift detected: tracked=%d, universe=%d",
                    tracked, universeCount);
            }
            // Sync tracked count to universe count to correct drift
            trackedPlayerCount = universeCount;
        }

        // Use higher value as fallback in case universe query fails momentarily
        return Math.max(universeCount, tracked);
    }

    /**
     * Builds a JSON array of all online players with their username, UUID, and current world.
     */
    private JsonArray buildPlayerList() {
        JsonArray players = new JsonArray();
        try {
            for (var entry : Universe.get().getWorlds().entrySet()) {
                var world = entry.getValue();
                for (var ref : world.getPlayerRefs()) {
                    JsonObject player = new JsonObject();
                    player.addProperty("username", ref.getUsername());
                    player.addProperty("uuid", ref.getUuid().toString());
                    player.addProperty("world", world.getName());
                    players.add(player);
                }
            }
        } catch (Exception e) {
            if (config.getHytaleChartsDebug()) {
                getLogger().at(java.util.logging.Level.WARNING).log("Failed to build player list: %s", e.getMessage());
            }
        }
        return players;
    }

    private void sendPromoLinkToPlayer(PlayerRef playerRef) {
        try {
            String messageText = config.getHytaleChartsPromoMessage() != null && !config.getHytaleChartsPromoMessage().isEmpty()
                ? config.getHytaleChartsPromoMessage()
                : "Vote for our server on HytaleCharts!";
            String linkUrl = config.getHytaleChartsPromoUrl() != null && !config.getHytaleChartsPromoUrl().isEmpty()
                ? config.getHytaleChartsPromoUrl()
                : "https://hytalecharts.com";

            Message promoLink;
            if (config.getHytaleChartsPromoPrefix() != null && !config.getHytaleChartsPromoPrefix().isEmpty()) {
                Message prefix = Message.raw(config.getHytaleChartsPromoPrefix()).color(Color.WHITE);
                Message linkText = Message.raw(messageText).color(Color.WHITE).link(linkUrl);
                promoLink = prefix.insert(linkText);
            } else {
                promoLink = Message.raw(messageText).color(Color.WHITE).link(linkUrl);
            }
            playerRef.sendMessage(promoLink);

            if (config.getHytaleChartsDebug()) {
                getLogger().at(java.util.logging.Level.INFO).log("Sent promo link to %s", playerRef.getUsername());
            }
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.WARNING).log("Failed to send promo link to %s: %s", playerRef.getUsername(), e.getMessage());
        }
    }

    private void broadcastPromoLink() {
        if (trackedPlayerCount == 0) return; // Don't broadcast if no one is online

        try {
            String messageText = config.getHytaleChartsPromoMessage() != null && !config.getHytaleChartsPromoMessage().isEmpty()
                ? config.getHytaleChartsPromoMessage()
                : "Vote for our server on HytaleCharts!";
            String linkUrl = config.getHytaleChartsPromoUrl() != null && !config.getHytaleChartsPromoUrl().isEmpty()
                ? config.getHytaleChartsPromoUrl()
                : "https://hytalecharts.com";

            Message promoLink;
            if (config.getHytaleChartsPromoPrefix() != null && !config.getHytaleChartsPromoPrefix().isEmpty()) {
                Message prefix = Message.raw(config.getHytaleChartsPromoPrefix()).color(Color.WHITE);
                Message linkText = Message.raw(messageText).color(Color.WHITE).link(linkUrl);
                promoLink = prefix.insert(linkText);
            } else {
                promoLink = Message.raw(messageText).color(Color.WHITE).link(linkUrl);
            }
            Universe.get().sendMessage(promoLink);

            if (config.getHytaleChartsDebug()) {
                getLogger().at(java.util.logging.Level.INFO).log("Promo link broadcast to %d player(s)", trackedPlayerCount);
            }
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.WARNING).log("Failed to broadcast promo link: %s", e.getMessage());
        }
    }

    private void sendHeartbeat() {
        try {
            int playerCount = getReliablePlayerCount();
            int maxPlayers = HytaleServer.get().getConfig().getMaxPlayers();

            JsonObject body = new JsonObject();
            body.addProperty("secret", config.getHytaleChartsSecret());
            body.addProperty("player_count", playerCount);
            body.addProperty("max_players", maxPlayers);
            body.addProperty("version", VERSION + "-takaro");

            // Include player list with usernames, UUIDs, and worlds
            body.add("players", buildPlayerList());

            HttpRequest request = HttpRequest.newBuilder()
                .uri(URI.create(HYTALECHARTS_API_URL))
                .header("Content-Type", "application/json")
                .header("User-Agent", "HytaleCharts-Plugin/" + VERSION)
                .header("takaro", "hytalecharts-XPJULXPTHN-" + VERSION + "-takaro")
                .POST(HttpRequest.BodyPublishers.ofString(body.toString()))
                .timeout(Duration.ofSeconds(10))
                .build();

            HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());

            if (response.statusCode() >= 200 && response.statusCode() < 300) {
                if (config.getHytaleChartsDebug()) {
                    getLogger().at(java.util.logging.Level.INFO).log("Heartbeat sent: %d/%d players, response: %d",
                        playerCount, maxPlayers, response.statusCode());
                }
            } else if (response.statusCode() == 429) {
                getLogger().at(java.util.logging.Level.WARNING).log("Heartbeat rate limited (HTTP 429) - this is normal after restarts, will retry in %d seconds",
                    HEARTBEAT_INTERVAL_SECONDS);
            } else if (response.statusCode() == 401) {
                getLogger().at(java.util.logging.Level.WARNING).log("Heartbeat failed: Invalid secret key - generate a new one at hytalecharts.com");
            } else {
                getLogger().at(java.util.logging.Level.WARNING).log("Heartbeat failed: HTTP %d", response.statusCode());
            }
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.WARNING).log("Heartbeat failed: %s", e.getMessage());
        }
    }

    @Override
    protected void shutdown() {
        super.shutdown();
        getLogger().at(java.util.logging.Level.INFO).log("Shutting down Takaro integration...");

        if (logHandler != null) {
            HytaleLoggerBackend.unsubscribe(logHandler.getLogBuffer());
            logHandler.stop();
        }

        if (telemetryScheduler != null) {
            telemetryScheduler.shutdownNow();
        }

        if (hytaleChartsScheduler != null && !hytaleChartsScheduler.isShutdown()) {
            hytaleChartsScheduler.shutdown();
        }

        if (webSocket != null) {
            webSocket.shutdown();
        }

        if (devWebSocket != null) {
            devWebSocket.shutdown();
        }

        if (hytaleApi != null) {
            hytaleApi.shutdown();
        }
    }

    public void handleTakaroRequest(TakaroWebSocket sourceWebSocket, String requestId, String action, JsonObject payload) {
        requestHandler.handleRequest(sourceWebSocket, requestId, action, payload);
    }

    public TakaroWebSocket getWebSocket() {
        return webSocket;
    }

    public TakaroWebSocket getDevWebSocket() {
        return devWebSocket;
    }

    public TakaroConfig getConfig() {
        return config;
    }

    public String getVersion() {
        return VERSION;
    }

    /**
     * Send game event to all active Takaro connections (production and dev if enabled)
     * @param eventType Type of event
     * @param data Event data
     */
    public void sendGameEventToAll(String eventType, Map<String, Object> data) {
        // Send to production
        if (webSocket != null) {
            webSocket.sendGameEvent(eventType, data);
        }
        // Send to dev (if enabled and connected)
        // Dev Takaro doesn't support log events or chat events - only send player-connected/disconnected
        if (devWebSocket != null && !eventType.equals("log") && !eventType.equals("chat-message")) {
            devWebSocket.sendGameEvent(eventType, data);
        }
    }

    /**
     * Get a player's name color from cache
     * @param uuid Player UUID
     * @return Color code (e.g., "gold", "ff0000") or null if not set
     */
    public String getPlayerNameColor(String uuid) {
        return playerNameColors.get(uuid);
    }

    /**
     * Set a player's name color in cache
     * Called by Takaro via setPlayerNameColor action
     * @param uuid Player UUID
     * @param colorCode Color code (e.g., "gold", "ff0000")
     */
    public void setPlayerNameColor(String uuid, String colorCode) {
        if (colorCode == null || colorCode.isEmpty()) {
            playerNameColors.remove(uuid);
            getLogger().at(java.util.logging.Level.INFO).log("Removed name color for player: " + uuid);
        } else {
            playerNameColors.put(uuid, colorCode);
            getLogger().at(java.util.logging.Level.INFO).log("Set name color for player " + uuid + ": " + colorCode);
        }
    }
}
