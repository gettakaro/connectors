package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.PlayerInfo;
import io.takaro.zomboid.core.model.PlayerLocation;
import io.takaro.zomboid.core.model.PlayerPositionSample;

import java.util.List;

public interface EventEmitter {
    void emitPlayerConnected(PlayerInfo player);
    void emitPlayerDisconnected(PlayerInfo player);
    void emitChatMessage(PlayerInfo player, String channel, String message);
    default void emitChatMessage(PlayerInfo player, String channel, String message, PlayerLocation position) {
        emitChatMessage(player, channel, message);
    }
    default void emitPlayerPositions(List<PlayerPositionSample> positions) {}
    default void emitPlayerPositions(List<PlayerPositionSample> positions, long capturedAtNanos) {
        emitPlayerPositions(positions);
    }
    default void setPositionPushAvailable(boolean available) {}
    default boolean canPushPositions() { return false; }
    void emitPlayerDeath(PlayerInfo player, PlayerInfo attacker, double x, double y, double z, String dimension);
    void emitEntityKilled(PlayerInfo player, String entityCode, String weaponCode);
    void emitLog(String message);

    /** The adapter's capabilities changed (for example the map image is ready): resend the manifest. */
    default void capabilitiesChanged() {}
}
