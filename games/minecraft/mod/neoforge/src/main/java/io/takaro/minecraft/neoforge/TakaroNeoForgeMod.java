package io.takaro.minecraft.neoforge;

import io.takaro.minecraft.core.EventEmitter;
import io.takaro.minecraft.core.config.ConfigFile;
import io.takaro.minecraft.core.config.PropertiesConfigFormat;
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

import java.nio.file.Path;

@Mod("takaro")
public class TakaroNeoForgeMod {

    private static final Logger LOGGER = LogManager.getLogger("Takaro");
    private volatile TakaroConnector connector;
    private volatile NeoForgeGameAdapter adapter;

    public TakaroNeoForgeMod() {
        NeoForge.EVENT_BUS.register(this);
    }

    @SubscribeEvent
    public void onServerStarted(ServerStartedEvent event) {
        MinecraftServer server = event.getServer();
        // Relative to the server directory, where NeoForge keeps config/.
        Path configPath = Path.of("config", "takaro.properties");
        adapter = new NeoForgeGameAdapter(LOGGER, server);
        connector = new TakaroConnector(adapter, new ConfigFile(configPath, new PropertiesConfigFormat(), adapter));
        connector.start();
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
}
