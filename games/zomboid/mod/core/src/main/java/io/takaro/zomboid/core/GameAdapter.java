package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.*;

import java.util.List;

public interface GameAdapter {
    void logInfo(String msg);
    void logWarning(String msg);
    default void logDebug(String msg) {}

    /** The game server's display name, used as the default chat sender name. */
    default String getServerName() { return null; }
    /** The running game build for {@code game.profile}, or {@code null} when unknown. */
    default String getGameVersion() { return null; }
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

    // Map (protocol v1 only)

    /**
     * The map Dimensions to declare in the {@code map} capability. Empty (the default) declares
     * no map. May change at runtime; the adapter then calls
     * {@link EventEmitter#capabilitiesChanged()} so the manifest is sent again.
     */
    default List<MapDimension> getMapDimensions() { return List.of(); }

    /**
     * Bytes {@code offset..offset+length-1} (or up to the end) of a Dimension's map image. Throws
     * {@link ProtocolException} with {@code not_found} for an unknown Dimension and
     * {@code invalid_args} for an offset past the end. Called off the game thread.
     */
    default MapImageChunk readMapImage(String dimension, long offset, int length) {
        throw new ProtocolException(ProtocolException.NOT_FOUND, "No map image for dimension " + dimension);
    }

    // Event wiring
    void setEventEmitter(EventEmitter emitter);
}
