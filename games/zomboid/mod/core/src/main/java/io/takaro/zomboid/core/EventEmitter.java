package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.PlayerInfo;

public interface EventEmitter {
    void emitPlayerConnected(PlayerInfo player);
    void emitPlayerDisconnected(PlayerInfo player);
    void emitChatMessage(PlayerInfo player, String channel, String message);
    void emitPlayerDeath(PlayerInfo player, PlayerInfo attacker, double x, double y, double z, String dimension);
    void emitEntityKilled(PlayerInfo player, String entityCode, String weaponCode);
    void emitLog(String message);
}
