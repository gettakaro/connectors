package io.takaro.minecraft.paper;

import io.takaro.minecraft.core.EventEmitter;
import io.takaro.minecraft.core.GameAdapter;
import io.takaro.minecraft.core.TakaroConfig;
import io.takaro.minecraft.core.TakaroConnector;
import io.takaro.minecraft.core.model.*;
import net.kyori.adventure.text.Component;
import org.bukkit.*;
import org.bukkit.ban.ProfileBanList;
import org.bukkit.entity.EntityType;
import org.bukkit.entity.Player;
import org.bukkit.inventory.ItemStack;
import org.bukkit.plugin.java.JavaPlugin;

import java.time.Instant;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.Collectors;

public class TakaroPaperPlugin extends JavaPlugin implements GameAdapter {

    private TakaroConnector connector;
    private EventEmitter eventEmitter;
    private final ConcurrentHashMap<String, PlayerLocation> lastKnownLocations = new ConcurrentHashMap<>();

    @Override
    public void onEnable() {
        saveDefaultConfig();

        TakaroConfig config = new TakaroConfig();
        config.setWsUrl(getConfig().getString("takaro.websocket.url", ""));
        config.setIdentityToken(getConfig().getString("takaro.authentication.identity_token", ""));
        config.setRegistrationToken(getConfig().getString("takaro.authentication.registration_token", ""));
        config.setReconnectEnabled(getConfig().getBoolean("takaro.reconnect.enabled", true));
        config.setReconnectDelay(getConfig().getLong("takaro.reconnect.delay", 5000));
        config.setMaxReconnectDelay(getConfig().getLong("takaro.reconnect.max_delay", 300000));
        config.setBackoffMultiplier(getConfig().getDouble("takaro.reconnect.backoff_multiplier", 1.5));
        config.setDebugEnabled(getConfig().getBoolean("takaro.debug", false));
        config.applyEnvOverrides();

        if (config.getWsUrl() == null || config.getWsUrl().isEmpty()) {
            getLogger().warning("No WebSocket URL configured, skipping Takaro connection");
            return;
        }

        connector = new TakaroConnector(this, config);
        connector.connect();

        // Register event listeners
        getServer().getPluginManager().registerEvents(new TakaroPaperEventListener(this), this);
    }

    @Override
    public void onDisable() {
        if (connector != null) {
            connector.shutdown();
        }
    }

    public EventEmitter getEventEmitter() {
        return eventEmitter;
    }

    // --- GameAdapter: Logging ---

    @Override
    public void logInfo(String msg) {
        getLogger().info(msg);
    }

    @Override
    public void logWarning(String msg) {
        getLogger().warning(msg);
    }

    @Override
    public void logDebug(String msg) {
        getLogger().info("[DEBUG] " + msg);
    }

    @Override
    public void runOnMainThread(Runnable task) {
        Bukkit.getScheduler().runTask(this, task);
    }

    // --- GameAdapter: Player queries ---

    @Override
    public PlayerInfo getPlayer(String gameId) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        return player != null ? toPlayerInfo(player) : null;
    }

    @Override
    public List<PlayerInfo> getPlayers() {
        return Bukkit.getOnlinePlayers().stream()
                .map(this::toPlayerInfo)
                .collect(Collectors.toList());
    }

    @Override
    public PlayerLocation getPlayerLocation(String gameId) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        if (player == null) return lastKnownLocations.get(gameId);
        var playerLoc = player.getLocation();
        PlayerLocation loc = new PlayerLocation(playerLoc.getX(), playerLoc.getY(), playerLoc.getZ(), mapDimension(playerLoc.getWorld()));
        lastKnownLocations.put(gameId, loc);
        return loc;
    }

    @Override
    public List<InventoryItem> getPlayerInventory(String gameId) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        if (player == null) return Collections.emptyList();
        List<InventoryItem> items = new ArrayList<>();
        for (ItemStack stack : player.getInventory().getContents()) {
            if (stack != null && stack.getType() != Material.AIR) {
                items.add(new InventoryItem(
                        stack.getType().getKey().toString(),
                        stack.getType().name().toLowerCase(),
                        stack.getAmount(),
                        ""
                ));
            }
        }
        return items;
    }

    // --- GameAdapter: World queries ---

    @Override
    public List<GameItem> listItems() {
        List<GameItem> items = new ArrayList<>();
        for (Material mat : Material.values()) {
            if (mat.isItem() && !mat.isLegacy()) {
                items.add(new GameItem(
                        mat.getKey().toString(),
                        mat.name().toLowerCase(),
                        ""
                ));
            }
        }
        return items;
    }

    @Override
    public List<GameEntity> listEntities() {
        List<GameEntity> entities = new ArrayList<>();
        for (EntityType type : EntityType.values()) {
            if (type.isAlive() && type != EntityType.PLAYER) {
                String category = type.getEntityClass() != null
                        && org.bukkit.entity.Monster.class.isAssignableFrom(type.getEntityClass())
                        ? "hostile" : "friendly";
                entities.add(new GameEntity(
                        type.getKey().toString(),
                        type.name().toLowerCase(),
                        "",
                        category
                ));
            }
        }
        return entities;
    }

    @Override
    public List<GameLocation> listLocations() {
        return Collections.emptyList();
    }

    // --- GameAdapter: Player actions ---

    @Override
    public void giveItem(String gameId, String itemCode, int amount, String quality) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        if (player == null) return;
        Material mat = Material.matchMaterial(itemCode);
        if (mat == null) return;
        player.getInventory().addItem(new ItemStack(mat, amount));
    }

    @Override
    public void sendMessage(String message, String recipientGameId) {
        if (recipientGameId != null) {
            Player player = Bukkit.getPlayer(UUID.fromString(recipientGameId));
            if (player != null) {
                player.sendMessage(Component.text(message));
            }
        } else {
            Bukkit.broadcast(Component.text(message));
        }
    }

    @Override
    public CommandResult executeConsoleCommand(String command) {
        try {
            boolean success = Bukkit.dispatchCommand(Bukkit.getConsoleSender(), command);
            return new CommandResult(success, "", null);
        } catch (Exception e) {
            return new CommandResult(false, "", e.getMessage());
        }
    }

    @Override
    public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        if (player == null) return;
        org.bukkit.World world = dimension != null ? getWorldForDimension(dimension) : player.getWorld();
        if (world == null) world = player.getWorld();
        player.teleport(new Location(world, x, y, z));
    }

    @Override
    public void kickPlayer(String gameId, String reason) {
        Player player = Bukkit.getPlayer(UUID.fromString(gameId));
        if (player != null) {
            player.kick(Component.text(reason));
        }
    }

    @Override
    public void banPlayer(String gameId, String reason, String expiresAt) {
        ProfileBanList banList = Bukkit.getBanList(BanList.Type.PROFILE);
        UUID uuid = UUID.fromString(gameId);
        OfflinePlayer offlinePlayer = Bukkit.getOfflinePlayer(uuid);
        var profile = offlinePlayer.getPlayerProfile();

        Date expiry = null;
        if (expiresAt != null) {
            expiry = Date.from(Instant.parse(expiresAt));
        }
        banList.addBan(profile, reason, expiry, "Takaro");

        Player player = Bukkit.getPlayer(uuid);
        if (player != null) {
            player.kick(Component.text("Banned: " + reason));
        }
    }

    @Override
    public void unbanPlayer(String gameId) {
        ProfileBanList banList = Bukkit.getBanList(BanList.Type.PROFILE);
        OfflinePlayer offlinePlayer = Bukkit.getOfflinePlayer(UUID.fromString(gameId));
        banList.pardon(offlinePlayer.getPlayerProfile());
    }

    @Override
    public List<io.takaro.minecraft.core.model.BanEntry> listBans() {
        ProfileBanList banList = Bukkit.getBanList(BanList.Type.PROFILE);
        List<io.takaro.minecraft.core.model.BanEntry> bans = new ArrayList<>();
        for (org.bukkit.BanEntry<?> entry : banList.getEntries()) {
            // getBanTarget() returns PlayerProfile for ProfileBanList
            Object target = entry.getBanTarget();
            String uuid = "";
            String name = "";
            if (target instanceof com.destroystokyo.paper.profile.PlayerProfile profile) {
                uuid = profile.getId() != null ? profile.getId().toString() : "";
                name = profile.getName() != null ? profile.getName() : "";
            }
            String expiresAt = entry.getExpiration() != null ? entry.getExpiration().toInstant().toString() : null;
            bans.add(new io.takaro.minecraft.core.model.BanEntry(uuid, name, entry.getReason(), expiresAt));
        }
        return bans;
    }

    @Override
    public void shutdownServer() {
        Bukkit.shutdown();
    }

    @Override
    public void setEventEmitter(EventEmitter emitter) {
        this.eventEmitter = emitter;
    }

    // --- Helpers ---

    PlayerInfo toPlayerInfo(Player player) {
        return new PlayerInfo(
                player.getUniqueId().toString(),
                player.getName(),
                null, null, null,
                PlayerInfo.buildPlatformId(player.getUniqueId().toString()),
                player.getAddress() != null ? player.getAddress().getAddress().getHostAddress() : "",
                player.getPing()
        );
    }

    String mapDimension(org.bukkit.World world) {
        if (world == null) return "overworld";
        return switch (world.getEnvironment()) {
            case NORMAL -> "overworld";
            case NETHER -> "nether";
            case THE_END -> "the_end";
            default -> world.getName();
        };
    }

    private org.bukkit.World getWorldForDimension(String dimension) {
        return switch (dimension) {
            case "overworld" -> Bukkit.getWorlds().stream()
                    .filter(w -> w.getEnvironment() == org.bukkit.World.Environment.NORMAL)
                    .findFirst().orElse(null);
            case "nether" -> Bukkit.getWorlds().stream()
                    .filter(w -> w.getEnvironment() == org.bukkit.World.Environment.NETHER)
                    .findFirst().orElse(null);
            case "the_end" -> Bukkit.getWorlds().stream()
                    .filter(w -> w.getEnvironment() == org.bukkit.World.Environment.THE_END)
                    .findFirst().orElse(null);
            default -> Bukkit.getWorld(dimension);
        };
    }
}
