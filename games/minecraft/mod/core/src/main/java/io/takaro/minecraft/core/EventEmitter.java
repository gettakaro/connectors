package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.PlayerInfo;
import io.takaro.minecraft.core.model.PlayerLocation;
import io.takaro.minecraft.core.model.PositionSample;

import java.util.List;

public interface EventEmitter {
    void emitPlayerConnected(PlayerInfo player);
    void emitPlayerDisconnected(String gameId, String playerName);
    void emitChatMessage(String gameId, String playerName, String channel, String message);
    default void emitChatMessage(String gameId, String playerName, String channel, String message, PlayerLocation position) {
        emitChatMessage(gameId, playerName, channel, message);
    }
    default boolean acceptsPlayerPositions() { return false; }
    void emitPlayerPositions(List<PositionSample> positions);
    default void emitPlayerPositions(List<PositionSample> positions, long capturedAtNanos) {
        emitPlayerPositions(positions);
    }
    void emitPlayerDeath(String gameId, String playerName, String attackerGameId, String attackerName, double x, double y, double z, String dimension);
    void emitEntityKilled(String gameId, String playerName, String entityCode, String weaponCode);
    void emitLog(String message);
}
