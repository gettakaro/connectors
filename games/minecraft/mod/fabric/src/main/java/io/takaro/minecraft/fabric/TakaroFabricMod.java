package io.takaro.minecraft.fabric;

import io.takaro.minecraft.core.EventEmitter;
import io.takaro.minecraft.core.config.ConfigFile;
import io.takaro.minecraft.core.config.JsonConfigFormat;
import io.takaro.minecraft.core.TakaroConnector;
import io.takaro.minecraft.core.model.PlayerInfo;
import net.fabricmc.api.DedicatedServerModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.message.v1.ServerMessageEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.entity.event.v1.ServerLivingEntityEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;


import java.nio.file.Path;

public class TakaroFabricMod implements DedicatedServerModInitializer {

    private static final Logger LOGGER = LoggerFactory.getLogger("Takaro");
    private volatile TakaroConnector connector;
    private volatile FabricGameAdapter adapter;

    @Override
    public void onInitializeServer() {
        ServerLifecycleEvents.SERVER_STARTED.register(this::onServerStarted);
        ServerLifecycleEvents.SERVER_STOPPING.register(server -> {
            emitDisconnectForOnlinePlayers(server);
            if (connector != null) {
                connector.shutdown();
            }
        });

        // Register game event callbacks
        ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
            if (adapter == null) return;
            EventEmitter emitter = adapter.getEventEmitter();
            if (emitter == null) return;
            ServerPlayer player = handler.getPlayer();
            emitter.emitPlayerConnected(adapter.toPlayerInfo(player));
        });

        ServerPlayConnectionEvents.DISCONNECT.register((handler, server) -> {
            if (adapter == null) return;
            EventEmitter emitter = adapter.getEventEmitter();
            if (emitter == null) return;
            ServerPlayer player = handler.getPlayer();
            String gameId = player.getUUID().toString();
            adapter.getPlayerLocation(gameId); // warm cache before player is removed from list
            emitter.emitPlayerDisconnected(gameId, player.getGameProfile().name());
        });

        ServerMessageEvents.CHAT_MESSAGE.register((message, sender, params) -> {
            if (adapter == null) return;
            EventEmitter emitter = adapter.getEventEmitter();
            if (emitter == null) return;
            emitter.emitChatMessage(
                    sender.getUUID().toString(),
                    sender.getGameProfile().name(),
                    "global",
                    message.signedContent()
            );
        });

        ServerLivingEntityEvents.AFTER_DEATH.register((entity, damageSource) -> {
            if (adapter == null) return;
            EventEmitter emitter = adapter.getEventEmitter();
            if (emitter == null) return;

            if (entity instanceof ServerPlayer victim) {
                // Player death
                String attackerGameId = null;
                String attackerName = null;
                if (damageSource.getEntity() instanceof Player attacker) {
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
            } else if (damageSource.getEntity() instanceof ServerPlayer killer) {
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
        });
    }

    /**
     * The DISCONNECT event does not fire for players still online when the server stops,
     * so Takaro would keep them marked online forever. Emit player-disconnected for each
     * of them before the connector shuts down.
     */
    private void emitDisconnectForOnlinePlayers(MinecraftServer server) {
        if (adapter == null || server == null) return;
        EventEmitter emitter = adapter.getEventEmitter();
        if (emitter == null) return;
        if (server.getPlayerList() == null) return;
        int emitted = 0;
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            if (player == null) continue;
            try {
                String gameId = player.getUUID().toString();
                adapter.getPlayerLocation(gameId); // warm cache before player is removed from list
                emitter.emitPlayerDisconnected(gameId, player.getGameProfile().name());
                emitted++;
            } catch (Exception e) {
                LOGGER.warn("Failed to emit player-disconnected on shutdown: {}", e.getMessage());
            }
        }
        if (emitted > 0) {
            // give the frames time to reach Takaro before the socket is torn down
            try {
                Thread.sleep(750);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private void onServerStarted(MinecraftServer server) {
        Path configPath = FabricLoader.getInstance().getConfigDir().resolve("takaro.json");
        adapter = new FabricGameAdapter(LOGGER, server);
        connector = new TakaroConnector(adapter, new ConfigFile(configPath, new JsonConfigFormat(), adapter));
        connector.start();
    }
}
