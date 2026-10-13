package io.takaro.minecraft.core;

import io.takaro.minecraft.core.map.RegionRenderer;
import io.takaro.minecraft.core.map.WorldMap;
import io.takaro.minecraft.core.model.*;
import io.takaro.minecraft.core.target.RuntimeIdentity;

import java.util.List;

public interface GameAdapter {
    void logInfo(String msg);
    void logWarning(String msg);
    default void logDebug(String msg) {}
    void runOnMainThread(Runnable task);

    // Player queries
    PlayerInfo getPlayer(String gameId);
    List<PlayerInfo> getPlayers();
    PlayerLocation getPlayerLocation(String gameId);
    List<InventoryItem> getPlayerInventory(String gameId);

    // World queries
    List<GameItem> listItems();
    List<GameEntity> listEntities();
    List<GameLocation> listLocations();

    // Player actions
    void giveItem(String gameId, String itemCode, int amount, String quality);
    void sendMessage(String message, String recipientGameId);
    CommandResult executeConsoleCommand(String command);
    void teleportPlayer(String gameId, double x, double y, double z, String dimension);
    void kickPlayer(String gameId, String reason);
    void banPlayer(String gameId, String reason, String expiresAt);
    void unbanPlayer(String gameId);
    List<BanEntry> listBans();
    void shutdownServer();

    // Event wiring
    void setEventEmitter(EventEmitter emitter);

    /**
     * What this server actually is, when the platform can tell us.
     * Platforms that cannot are left unchecked by the target guard rather than guessed at.
     */
    default RuntimeIdentity getRuntimeIdentity() { return null; }

    /**
     * The dimensions the world map draws, each with the folder of its saved region files. The
     * ids must be the {@code dimension} values getPlayerLocation reports. Empty: no map.
     */
    default List<WorldMap.Dimension> mapDimensions() { return List.of(); }

    /** The map colour of each block, as a vanilla filled map draws it. Null: no map. */
    default RegionRenderer.BlockColors mapColors() { return null; }
}
