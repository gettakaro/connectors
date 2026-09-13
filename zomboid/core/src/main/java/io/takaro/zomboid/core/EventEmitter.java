package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.PlayerInfo;

public interface EventEmitter {
    void emitPlayerConnected(PlayerInfo player);
    void emitPlayerDisconnected(String gameId, String playerName);
    void emitChatMessage(String gameId, String playerName, String channel, String message);
    void emitPlayerDeath(String gameId, String playerName, String attackerGameId, String attackerName, double x, double y, double z, String dimension);
    void emitEntityKilled(String gameId, String playerName, String entityCode, String weaponCode);
    void emitLog(String message);
}
