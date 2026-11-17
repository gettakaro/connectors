package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.Capabilities;
import io.takaro.zomboid.core.EventEmitter;
import io.takaro.zomboid.core.model.PlayerInfo;
import io.takaro.zomboid.core.model.PlayerLocation;
import io.takaro.zomboid.core.model.PlayerPositionSample;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Captures complete rosters on the game thread and withdraws push while any position is unreadable. */
public final class PositionPublisher {
    private static final long RETRY_NANOS = TimeUnit.SECONDS.toNanos(Capabilities.POSITION_INTERVAL_SECONDS);
    private final Supplier<List<PlayerInfo>> players;
    private final Function<String, PlayerLocation> location;
    private final LongSupplier clock;
    private boolean unavailable;
    private boolean attempted;
    private long lastAttempt;

    public PositionPublisher(Supplier<List<PlayerInfo>> players, Function<String, PlayerLocation> location,
                             LongSupplier clock) {
        this.players = players;
        this.location = location;
        this.clock = clock;
    }

    public void tick(EventEmitter emitter) {
        boolean ready = emitter.canPushPositions();
        if (!unavailable && !ready) return;
        long now = clock.getAsLong();
        if (attempted && now - lastAttempt < RETRY_NANOS) return;
        attempted = true;
        lastAttempt = now;
        List<PlayerInfo> roster = players.get();
        List<PlayerPositionSample> batch = new ArrayList<>();
        var seen = new HashSet<String>();
        boolean complete = roster != null && roster.size() <= 1000;
        if (complete) for (PlayerInfo player : roster) {
            if (player == null || player.gameId() == null || player.gameId().isEmpty()
                    || !seen.add(player.gameId())) { complete = false; break; }
            PlayerLocation loc = location.apply(player.gameId());
            if (loc == null || !Double.isFinite(loc.x()) || !Double.isFinite(loc.y())
                    || !Double.isFinite(loc.z())) { complete = false; break; }
            batch.add(new PlayerPositionSample(player.gameId(), loc));
        }
        unavailable = !complete;
        emitter.setPositionPushAvailable(complete);
        if (complete && emitter.canPushPositions()) emitter.emitPlayerPositions(batch, now);
    }
}
