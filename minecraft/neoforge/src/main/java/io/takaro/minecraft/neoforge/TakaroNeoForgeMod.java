package io.takaro.minecraft.neoforge;

import io.takaro.minecraft.core.EventEmitter;
import io.takaro.minecraft.core.TakaroConfig;
import io.takaro.minecraft.core.TakaroConnector;
import io.takaro.minecraft.core.model.PlayerInfo;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.neoforged.bus.api.SubscribeEvent;
import net.neoforged.fml.common.Mod;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.event.ServerChatEvent;
import net.neoforged.neoforge.event.entity.living.LivingDeathEvent;
import net.neoforged.neoforge.event.entity.player.PlayerEvent;
import net.neoforged.neoforge.event.server.ServerStartedEvent;
import net.neoforged.neoforge.event.server.ServerStoppingEvent;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

@Mod("takaro")
public class TakaroNeoForgeMod {

    private static final Logger LOGGER = LogManager.getLogger("Takaro");
    private TakaroConnector connector;
    private NeoForgeGameAdapter adapter;

    public TakaroNeoForgeMod() {
        NeoForge.EVENT_BUS.register(this);
    }

    @SubscribeEvent
    public void onServerStarted(ServerStartedEvent event) {
        MinecraftServer server = event.getServer();
        Path configPath = Path.of("config", "takaro.properties");

        TakaroConfig config = loadConfig(configPath);
        if (config == null) {
            config = new TakaroConfig();
        }
        config.applyEnvOverrides();

        if (config.getWsUrl() == null || config.getWsUrl().isEmpty()) {
            LOGGER.warn("No WebSocket URL configured, skipping Takaro connection");
            return;
        }

        adapter = new NeoForgeGameAdapter(LOGGER, server);
        connector = new TakaroConnector(adapter, config);
        connector.connect();
    }

    @SubscribeEvent
    public void onServerStopping(ServerStoppingEvent event) {
        if (connector != null) {
            connector.shutdown();
        }
    }

    // --- Game Events ---

    @SubscribeEvent
    public void onPlayerLoggedIn(PlayerEvent.PlayerLoggedInEvent event) {
        if (adapter == null) return;
        EventEmitter emitter = adapter.getEventEmitter();
        if (emitter == null) return;
        if (event.getEntity() instanceof ServerPlayer player) {
            emitter.emitPlayerConnected(adapter.toPlayerInfo(player));
        }
    }

    @SubscribeEvent
    public void onPlayerLoggedOut(PlayerEvent.PlayerLoggedOutEvent event) {
        if (adapter == null) return;
        EventEmitter emitter = adapter.getEventEmitter();
        if (emitter == null) return;
        ServerPlayer player = (ServerPlayer) event.getEntity();
        String gameId = player.getUUID().toString();
        adapter.getPlayerLocation(gameId); // warm cache before player is removed from list
        emitter.emitPlayerDisconnected(gameId, player.getName().getString());
    }

    @SubscribeEvent
    public void onServerChat(ServerChatEvent event) {
        if (adapter == null) return;
        EventEmitter emitter = adapter.getEventEmitter();
        if (emitter == null) return;
        ServerPlayer player = event.getPlayer();
        emitter.emitChatMessage(
                player.getUUID().toString(),
                player.getGameProfile().name(),
                "global",
                event.getRawText()
        );
    }

    @SubscribeEvent
    public void onLivingDeath(LivingDeathEvent event) {
        if (adapter == null) return;
        EventEmitter emitter = adapter.getEventEmitter();
        if (emitter == null) return;
        LivingEntity entity = event.getEntity();

        if (entity instanceof ServerPlayer victim) {
            // Player death
            String attackerGameId = null;
            String attackerName = null;
            if (event.getSource().getEntity() instanceof Player attacker) {
                attackerGameId = attacker.getUUID().toString();
                attackerName = attacker.getGameProfile().name();
            }
            emitter.emitPlayerDeath(
                    victim.getUUID().toString(),
                    victim.getGameProfile().name(),
                    attackerGameId, attackerName,
                    victim.getX(), victim.getY(), victim.getZ(),
                    adapter.mapDimension(victim.level().dimension().identifier())
            );
        } else if (event.getSource().getEntity() instanceof ServerPlayer killer) {
            // Entity killed by player
            var entityKey = net.minecraft.core.registries.BuiltInRegistries.ENTITY_TYPE.getKey(entity.getType());
            String weaponCode = "";
            var mainHand = killer.getMainHandItem();
            if (!mainHand.isEmpty()) {
                var itemKey = net.minecraft.core.registries.BuiltInRegistries.ITEM.getKey(mainHand.getItem());
                weaponCode = itemKey != null ? itemKey.toString() : "";
            }
            emitter.emitEntityKilled(
                    killer.getUUID().toString(),
                    killer.getGameProfile().name(),
                    entityKey != null ? entityKey.toString() : "unknown",
                    weaponCode
            );
        }
    }

    // --- Config ---

    private TakaroConfig loadConfig(Path path) {
        if (!Files.exists(path)) {
            LOGGER.warn("Config file not found at {}, creating default...", path);
            createDefaultConfig(path);
            return null;
        }

        Properties props = new Properties();
        try (InputStream in = Files.newInputStream(path)) {
            props.load(in);
        } catch (IOException e) {
            LOGGER.error("Failed to load config: {}", e.getMessage());
            return null;
        }

        try {
            TakaroConfig config = new TakaroConfig();
            config.setWsUrl(props.getProperty("takaro.websocket.url", ""));
            config.setIdentityToken(props.getProperty("takaro.authentication.identity_token", ""));
            config.setRegistrationToken(props.getProperty("takaro.authentication.registration_token", ""));
            config.setReconnectEnabled(Boolean.parseBoolean(props.getProperty("takaro.reconnect.enabled", "true")));
            config.setReconnectDelay(Long.parseLong(props.getProperty("takaro.reconnect.delay", "5000")));
            config.setMaxReconnectDelay(Long.parseLong(props.getProperty("takaro.reconnect.max_delay", "300000")));
            config.setBackoffMultiplier(Double.parseDouble(props.getProperty("takaro.reconnect.backoff_multiplier", "1.5")));
            config.setDebugEnabled(Boolean.parseBoolean(props.getProperty("takaro.debug", "false")));
            return config;
        } catch (NumberFormatException e) {
            LOGGER.error("Invalid number in config: {}", e.getMessage());
            return null;
        }
    }

    private void createDefaultConfig(Path path) {
        try {
            Files.createDirectories(path.getParent());
            Files.writeString(path,
                    "takaro.websocket.url=\n" +
                    "takaro.authentication.identity_token=\n" +
                    "takaro.authentication.registration_token=\n" +
                    "takaro.reconnect.enabled=true\n" +
                    "takaro.reconnect.delay=5000\n" +
                    "takaro.reconnect.max_delay=300000\n" +
                    "takaro.reconnect.backoff_multiplier=1.5\n" +
                    "takaro.debug=false\n");
            LOGGER.info("Default config created at {}", path);
        } catch (IOException e) {
            LOGGER.error("Failed to create default config: {}", e.getMessage());
        }
    }
}
