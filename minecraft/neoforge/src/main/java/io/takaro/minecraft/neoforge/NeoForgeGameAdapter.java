package io.takaro.minecraft.neoforge;

import io.takaro.minecraft.core.EventEmitter;
import io.takaro.minecraft.core.GameAdapter;
import io.takaro.minecraft.core.model.*;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.players.NameAndId;
import net.minecraft.server.players.UserBanListEntry;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import org.apache.logging.log4j.Logger;

import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.Collectors;

public class NeoForgeGameAdapter implements GameAdapter {

    private final Logger logger;
    private final MinecraftServer server;
    private EventEmitter eventEmitter;
    private final ConcurrentHashMap<String, PlayerLocation> lastKnownLocations = new ConcurrentHashMap<>();

    public NeoForgeGameAdapter(Logger logger, MinecraftServer server) {
        this.logger = logger;
        this.server = server;
    }

    public EventEmitter getEventEmitter() {
        return eventEmitter;
    }

    @Override
    public void logInfo(String msg) { logger.info(msg); }

    @Override
    public void logWarning(String msg) { logger.warn(msg); }

    @Override
    public void logDebug(String msg) { logger.info("[DEBUG] " + msg); }

    @Override
    public void runOnMainThread(Runnable task) { server.execute(task); }

    @Override
    public PlayerInfo getPlayer(String gameId) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        return player != null ? toPlayerInfo(player) : null;
    }

    @Override
    public List<PlayerInfo> getPlayers() {
        return server.getPlayerList().getPlayers().stream()
                .map(this::toPlayerInfo)
                .collect(Collectors.toList());
    }

    @Override
    public PlayerLocation getPlayerLocation(String gameId) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        if (player == null) return lastKnownLocations.get(gameId);
        PlayerLocation loc = new PlayerLocation(
                player.getX(), player.getY(), player.getZ(),
                mapDimension(player.level().dimension().identifier())
        );
        lastKnownLocations.put(gameId, loc);
        return loc;
    }

    @Override
    public List<InventoryItem> getPlayerInventory(String gameId) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        if (player == null) return Collections.emptyList();
        List<InventoryItem> items = new ArrayList<>();
        for (ItemStack stack : player.getInventory().getNonEquipmentItems()) {
            if (!stack.isEmpty()) {
                var key = BuiltInRegistries.ITEM.getKey(stack.getItem());
                items.add(new InventoryItem(
                        key != null ? key.toString() : "unknown",
                        stack.getHoverName().getString(),
                        stack.getCount(),
                        ""
                ));
            }
        }
        return items;
    }

    @Override
    public List<GameItem> listItems() {
        List<GameItem> items = new ArrayList<>();
        BuiltInRegistries.ITEM.forEach(item -> {
            if (item != Items.AIR) {
                var key = BuiltInRegistries.ITEM.getKey(item);
                items.add(new GameItem(
                        key.toString(),
                        item.getName().getString(),
                        ""
                ));
            }
        });
        return items;
    }

    @Override
    public List<GameEntity> listEntities() {
        List<GameEntity> entities = new ArrayList<>();
        BuiltInRegistries.ENTITY_TYPE.forEach(type -> {
            var key = BuiltInRegistries.ENTITY_TYPE.getKey(type);
            String category = type.getCategory().isFriendly() ? "friendly" : "hostile";
            entities.add(new GameEntity(
                    key.toString(),
                    type.getDescription().getString(),
                    "",
                    category
            ));
        });
        return entities;
    }

    @Override
    public List<GameLocation> listLocations() {
        return Collections.emptyList();
    }

    @Override
    public void giveItem(String gameId, String itemCode, int amount, String quality) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        if (player == null) return;
        var itemHolder = BuiltInRegistries.ITEM.get(Identifier.parse(itemCode));
        if (itemHolder.isEmpty() || itemHolder.get().value() == Items.AIR) return;
        ItemStack stack = new ItemStack(itemHolder.get(), amount);
        player.getInventory().add(stack);
    }

    @Override
    public void sendMessage(String message, String recipientGameId) {
        var component = net.minecraft.network.chat.Component.literal(message);
        if (recipientGameId != null) {
            ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(recipientGameId));
            if (player != null) {
                player.sendSystemMessage(component);
            }
        } else {
            server.getPlayerList().broadcastSystemMessage(component, false);
        }
    }

    @Override
    public CommandResult executeConsoleCommand(String command) {
        try {
            var source = server.createCommandSourceStack();
            server.getCommands().performPrefixedCommand(source, command);
            return new CommandResult(true, "", null);
        } catch (Exception e) {
            return new CommandResult(false, "", e.getMessage());
        }
    }

    @Override
    public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        if (player == null) return;
        ServerLevel level = dimension != null ? getLevelForDimension(dimension) : (ServerLevel) player.level();
        if (level == null) level = (ServerLevel) player.level();
        player.teleportTo(level, x, y, z, Set.of(), player.getYRot(), player.getXRot(), false);
    }

    @Override
    public void kickPlayer(String gameId, String reason) {
        ServerPlayer player = server.getPlayerList().getPlayer(UUID.fromString(gameId));
        if (player != null) {
            player.connection.disconnect(net.minecraft.network.chat.Component.literal(reason));
        }
    }

    @Override
    public void banPlayer(String gameId, String reason, String expiresAt) {
        UUID uuid = UUID.fromString(gameId);
        ServerPlayer onlinePlayer = server.getPlayerList().getPlayer(uuid);
        String playerName = onlinePlayer != null ? onlinePlayer.getGameProfile().name() : "";
        var nameAndId = new NameAndId(uuid, playerName);
        Date expiry = null;
        if (expiresAt != null) {
            expiry = Date.from(java.time.Instant.parse(expiresAt));
        }
        var entry = new UserBanListEntry(nameAndId, null, "Takaro", expiry, reason);
        server.getPlayerList().getBans().add(entry);
        // Kick if online
        if (onlinePlayer != null) {
            onlinePlayer.connection.disconnect(net.minecraft.network.chat.Component.literal("Banned: " + reason));
        }
    }

    @Override
    public void unbanPlayer(String gameId) {
        UUID uuid = UUID.fromString(gameId);
        var nameAndId = new NameAndId(uuid, "");
        server.getPlayerList().getBans().remove(nameAndId);
    }

    @Override
    public List<BanEntry> listBans() {
        List<BanEntry> bans = new ArrayList<>();
        for (var entry : server.getPlayerList().getBans().getEntries()) {
            var user = entry.getUser();
            if (user == null) continue;
            String expiresAt = entry.getExpires() != null ? entry.getExpires().toInstant().toString() : null;
            bans.add(new BanEntry(
                    user.id().toString(),
                    user.name(),
                    entry.getReason(),
                    expiresAt
            ));
        }
        return bans;
    }

    @Override
    public void shutdownServer() {
        server.halt(false);
    }

    @Override
    public void setEventEmitter(EventEmitter emitter) {
        this.eventEmitter = emitter;
    }

    // --- Helpers ---

    PlayerInfo toPlayerInfo(ServerPlayer player) {
        String ip = player.getIpAddress();
        return new PlayerInfo(
                player.getUUID().toString(),
                player.getGameProfile().name(),
                null, null, null,
                PlayerInfo.buildPlatformId(player.getUUID().toString()),
                ip != null ? ip : "",
                player.connection.latency()
        );
    }

    String mapDimension(Identifier dimensionId) {
        String path = dimensionId.toString();
        return switch (path) {
            case "minecraft:overworld" -> "overworld";
            case "minecraft:the_nether" -> "nether";
            case "minecraft:the_end" -> "the_end";
            default -> path;
        };
    }

    private ServerLevel getLevelForDimension(String dimension) {
        for (ServerLevel level : server.getAllLevels()) {
            if (mapDimension(level.dimension().identifier()).equals(dimension)) {
                return level;
            }
        }
        return null;
    }
}
