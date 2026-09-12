package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.*;

import java.util.List;

public interface GameAdapter {
    void logInfo(String msg);
    void logWarning(String msg);
    default void logDebug(String msg) {}
    void runOnMainThread(Runnable task);

    /**
     * Called every time the Takaro WebSocket opens (initial connect and every
     * reconnect). Lets an adapter re-seed its player registry without emitting
     * connect/disconnect events, so a reconnect does not produce a duplicate
     * event storm. Default no-op keeps existing adapters source-compatible.
     */
    default void onConnectionEstablished() {}

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
}
