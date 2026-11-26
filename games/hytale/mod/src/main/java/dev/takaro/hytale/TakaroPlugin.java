package dev.takaro.hytale;

import com.google.gson.JsonObject;
import com.hypixel.hytale.server.core.HytaleServer;
import com.hypixel.hytale.server.core.plugin.JavaPlugin;
import com.hypixel.hytale.server.core.plugin.JavaPluginInit;
import com.hypixel.hytale.server.core.universe.Universe;
import com.hypixel.hytale.logger.backend.HytaleLoggerBackend;
import dev.takaro.hytale.commands.TakaroDebugCommand;
import dev.takaro.hytale.config.TakaroConfig;
import dev.takaro.hytale.events.ChatEventListener;
import dev.takaro.hytale.events.EntityDeathSystem;
import dev.takaro.hytale.events.ProjectileWeaponSystem;
import dev.takaro.hytale.events.ModernProjectileWeaponSystem;
import dev.takaro.hytale.events.PlayerDeathSystem;
import dev.takaro.hytale.events.PlayerEventListener;
import dev.takaro.hytale.events.TakaroLogHandler;
import dev.takaro.hytale.handlers.TakaroRequestHandler;
import dev.takaro.hytale.state.KnownPlayers;
import dev.takaro.hytale.websocket.TakaroWebSocket;

import javax.annotation.Nonnull;
import java.io.File;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class TakaroPlugin extends JavaPlugin {
    /** The version the build stamped into version.properties; "dev" for an unstamped build. */
    private static final String VERSION = stampedVersionOrDev();

    private static String stampedVersionOrDev() {
        String v = dev.takaro.hytale.protocol.GameProfile.stampedVersion();
        return v == null ? "dev" : v;
    }

    private TakaroConfig config;
    private TakaroWebSocket webSocket;
    private TakaroRequestHandler requestHandler;
    private ChatEventListener chatListener;
    private PlayerEventListener playerListener;
    private PlayerDeathSystem deathSystem;
    private EntityDeathSystem entityDeathSystem;
    private ProjectileWeaponSystem projectileWeaponSystem;
    private TakaroLogHandler logHandler;
    private KnownPlayers knownPlayers;
    private java.util.concurrent.ExecutorService requestExecutor;
    private dev.takaro.hytale.map.WorldMapService worldMap;

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

        // Ledger of players this server has seen, so offline lookups and listBans can
        // answer with a real name instead of nothing.
        knownPlayers = new KnownPlayers(configFile.toPath().resolveSibling("known-players.json"));

        // Initialize request handler
        requestHandler = new TakaroRequestHandler(this);

        // Requests run here, never on the WebSocket reader thread.
        requestExecutor = new java.util.concurrent.ThreadPoolExecutor(
            2, 8, 60L, TimeUnit.SECONDS,
            new java.util.concurrent.LinkedBlockingQueue<>(256),
            r -> {
                Thread t = new Thread(r, "Takaro-Request");
                t.setDaemon(true);
                return t;
            });

        // Initialize event listeners
        chatListener = new ChatEventListener(this);
        playerListener = new PlayerEventListener(this);
        deathSystem = new PlayerDeathSystem(this);
        projectileWeaponSystem = new ProjectileWeaponSystem(this);
        entityDeathSystem = new EntityDeathSystem(this, projectileWeaponSystem);
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

            // Notify Takaro only once the player has a world entity and a readable position.
            this.getEventRegistry().registerGlobal(
                com.hypixel.hytale.server.core.event.events.player.PlayerReadyEvent.class,
                playerListener::onPlayerReady
            );
            getLogger().at(java.util.logging.Level.INFO).log("Registered PlayerReadyEvent handler");

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

            this.getEntityStoreRegistry().registerSystem(projectileWeaponSystem);
            getLogger().at(java.util.logging.Level.INFO).log("Registered ProjectileWeaponSystem");

            this.getEntityStoreRegistry().registerSystem(new ModernProjectileWeaponSystem(projectileWeaponSystem));
            getLogger().at(java.util.logging.Level.INFO).log("Registered ModernProjectileWeaponSystem");

            this.getEntityStoreRegistry().registerSystem(entityDeathSystem);
            getLogger().at(java.util.logging.Level.INFO).log("Registered EntityDeathSystem (entity-killed)");

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

        // Exactly one Takaro connection, to the URL the config resolves (public Takaro by default).
        try {
            webSocket = new TakaroWebSocket(this, config);
            webSocket.connect();
            getLogger().at(java.util.logging.Level.INFO).log("Connecting to Takaro at " + config.getWsUrl());
        } catch (Exception e) {
            getLogger().at(java.util.logging.Level.SEVERE).log("Failed to start WebSocket connection: " + e.getMessage());
            e.printStackTrace();
        }

        // Start log forwarding to Takaro
        logHandler.start();

        // Draw the world map for getMapInfo/getMapTile in the background
        if (config.isMapEnabled()) {
            worldMap = new dev.takaro.hytale.map.WorldMapService(this, getFile().getParent().resolve("HytaleTakaroMod"));
            worldMap.start();
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

        if (requestExecutor != null) {
            requestExecutor.shutdownNow();
        }

        if (worldMap != null) {
            worldMap.stop();
        }

        if (webSocket != null) {
            webSocket.shutdown();
        }
    }

    /**
     * Handle a Takaro request off the WebSocket reader thread.
     *
     * <p>Several handlers hand work to a world thread and block on a future while waiting for
     * the result. Doing that on the reader thread stalls ping/pong and every other request for
     * the duration; a slow or stuck world would look to Takaro like a dead server. The work is
     * therefore dispatched to a small bounded pool, and the reader thread returns immediately.
     */
    public void handleTakaroRequest(TakaroWebSocket sourceWebSocket, String requestId, String action, JsonObject payload) {
        try {
            requestExecutor.execute(() -> requestHandler.handleRequest(sourceWebSocket, requestId, action, payload));
        } catch (java.util.concurrent.RejectedExecutionException e) {
            getLogger().at(java.util.logging.Level.SEVERE).log(
                "Request executor is saturated, running '" + action + "' inline: " + e.getMessage());
            requestHandler.handleRequest(sourceWebSocket, requestId, action, payload);
        }
    }

    public TakaroWebSocket getWebSocket() {
        return webSocket;
    }

    /** The world map service, or null when MAP_ENABLED=false. */
    public dev.takaro.hytale.map.WorldMapService getWorldMap() {
        return worldMap;
    }

    public KnownPlayers getKnownPlayers() {
        return knownPlayers;
    }

    public PlayerEventListener getPlayerListener() {
        return playerListener;
    }

    public TakaroConfig getConfig() {
        return config;
    }

    public String getVersion() {
        return VERSION;
    }

    /** The running Hytale server build, or null when the server does not report one. */
    public String getServerVersion() {
        try {
            String reported = com.hypixel.hytale.common.util.java.ManifestUtil.getVersion();
            return reported == null || reported.isBlank() ? null : reported.trim();
        } catch (Exception | LinkageError e) {
            return null;
        }
    }

    /**
     * The protocol v1 map Dimension a position in {@code worldName} belongs to, or null (protocol
     * 0, map not declared yet, or a world the map does not draw).
     */
    public String mapDimensionFor(String worldName) {
        TakaroWebSocket ws = webSocket;
        return ws == null ? null : ws.dimensionFor(worldName);
    }

    /**
     * Send a game event to the Takaro connection
     * @param eventType Type of event
     * @param data Event data
     */
    public void sendGameEventToAll(String eventType, Map<String, Object> data) {
        if (webSocket != null) {
            webSocket.sendGameEvent(eventType, data);
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
