package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.EventEmitter;
import io.takaro.zomboid.core.model.PlayerInfo;

import java.time.Instant;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Diffs the online-player set each cycle to emit player-connected /
 * player-disconnected events, keeps {@link PlayerRegistry} current, and expires
 * timed bans from {@link BanStore}.
 *
 * <p>Runs entirely on the main thread (invoked from the tick hook). The connect
 * and disconnect advice set a dirty flag so a join/leave reconciles on the next
 * tick instead of waiting for the wall-clock cadence.
 */
public final class Reconciler {

    private final PlayerRegistry registry;
    private final BanStore banStore;
    private volatile EventEmitter emitter;

    private Map<String, PlayerInfo> previous = new LinkedHashMap<>();
    private volatile boolean dirty;
    private boolean seeded;

    public Reconciler(PlayerRegistry registry, BanStore banStore) {
        this.registry = registry;
        this.banStore = banStore;
    }

    public void setEmitter(EventEmitter emitter) {
        this.emitter = emitter;
    }

    public void markDirty() {
        this.dirty = true;
    }

    public boolean isDirty() {
        return dirty;
    }

    /** Snapshot current players without emitting — used on (re)connect to avoid duplicate events. */
    public void reseed() {
        previous = snapshotByGameId();
        registry.set(new ArrayList<>(previous.values()));
        seeded = true;
        dirty = false;
        AgentLog.log("reconciler: reseeded with " + previous.size() + " player(s), no events emitted");
    }

    /** Diff players, emit connect/disconnect, refresh registry, expire timed bans. */
    public void tick() {
        dirty = false;
        Map<String, PlayerInfo> current = snapshotByGameId();

        if (!seeded) {
            // First pass before any WS connect: seed silently.
            previous = current;
            registry.set(new ArrayList<>(current.values()));
            seeded = true;
            return;
        }

        EventEmitter e = emitter;
        if (e != null) {
            for (Map.Entry<String, PlayerInfo> entry : current.entrySet()) {
                if (!previous.containsKey(entry.getKey())) {
                    AgentLog.log("event: player-connected " + entry.getKey());
                    e.emitPlayerConnected(entry.getValue());
                }
            }
            for (Map.Entry<String, PlayerInfo> entry : previous.entrySet()) {
                if (!current.containsKey(entry.getKey())) {
                    AgentLog.log("event: player-disconnected " + entry.getKey());
                    e.emitPlayerDisconnected(entry.getValue().gameId(), entry.getValue().name());
                }
            }
        }

        previous = current;
        registry.set(new ArrayList<>(current.values()));

        expireBans();
    }

    private Map<String, PlayerInfo> snapshotByGameId() {
        Map<String, PlayerInfo> map = new LinkedHashMap<>();
        for (PlayerInfo p : Pz.getPlayers()) {
            map.put(p.gameId(), p);
        }
        return map;
    }

    private void expireBans() {
        List<BanStore.Ban> expired = banStore.expired(Instant.now());
        for (BanStore.Ban b : expired) {
            AgentLog.log("reconciler: timed ban expired for " + b.gameId + " — unbanning");
            try {
                Pz.unbanUser(b.gameId, b.steamId);
            } catch (Throwable t) {
                AgentLog.error("reconciler: unban-on-expiry failed for " + b.gameId, t);
            }
            banStore.remove(b.gameId);
        }
    }
}
