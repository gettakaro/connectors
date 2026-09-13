package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.PlayerInfo;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * An immutable snapshot of the online players, keyed by gameId (username).
 *
 * <p>Written on the main thread by the {@link Reconciler}; read from the
 * WebSocket thread (e.g. to resolve a chat author's display name without
 * touching {@code GameServer} off-thread).
 */
public final class PlayerRegistry {

    private volatile Map<String, PlayerInfo> snapshot = Map.of();

    public void set(List<PlayerInfo> players) {
        Map<String, PlayerInfo> next = new LinkedHashMap<>();
        for (PlayerInfo p : players) {
            next.put(p.gameId(), p);
        }
        this.snapshot = Map.copyOf(next);
    }

    public PlayerInfo get(String gameId) {
        return snapshot.get(gameId);
    }

    public Map<String, PlayerInfo> all() {
        return snapshot;
    }
}
