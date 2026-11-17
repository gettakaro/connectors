package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.PlayerInfo;
import io.takaro.minecraft.core.model.PlayerLocation;
import io.takaro.minecraft.core.model.PositionSample;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.LongSupplier;

/** Called from each loader's server tick, where its game API can be read safely. */
public final class PositionPublisher {
    private static final long INTERVAL_NANOS = TimeUnit.SECONDS.toNanos(TakaroWebSocketClient.POSITION_INTERVAL_SECONDS);
    private final LongSupplier clock;
    private long lastPublishedNanos;
    private boolean published;

    public PositionPublisher() { this(System::nanoTime); }

    PositionPublisher(LongSupplier clock) { this.clock = clock; }

    public void capture(GameAdapter adapter, EventEmitter emitter) {
        if (emitter == null || !emitter.acceptsPlayerPositions()) return;
        long now = clock.getAsLong();
        if (published && now - lastPublishedNanos < INTERVAL_NANOS) return;
        List<PlayerInfo> players = adapter.getPlayers();
        if (players.size() > 1000) {
            adapter.logWarning("Cannot push player positions: more than 1000 players are online");
            return;
        }
        List<PositionSample> positions = new ArrayList<>(players.size());
        for (PlayerInfo player : players) {
            PlayerLocation location = adapter.getPlayerLocation(player.gameId());
            if (location != null) positions.add(new PositionSample(player.gameId(), location));
        }
        lastPublishedNanos = clock.getAsLong();
        published = true;
        emitter.emitPlayerPositions(positions, now);
    }
}
