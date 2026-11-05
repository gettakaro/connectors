package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.ProgressionTrack;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The last perk reading per player, by gameId. Written on the main thread by the
 * {@link Reconciler}; read from the WebSocket thread, so Takaro's poll on every player sync costs
 * the game thread nothing. A reading outlives the player's session, and is empty after a restart
 * until the player's next reading.
 */
public final class Progression {

    private final Map<String, List<ProgressionTrack>> readings = new ConcurrentHashMap<>();

    public void put(String gameId, List<ProgressionTrack> tracks) {
        readings.put(gameId, List.copyOf(tracks));
    }

    /** The player's tracks, or null when this server has no reading for them. */
    public List<ProgressionTrack> get(String gameId) {
        return readings.get(gameId);
    }
}
