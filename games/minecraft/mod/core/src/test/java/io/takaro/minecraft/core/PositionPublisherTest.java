package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.PlayerInfo;
import io.takaro.minecraft.core.model.PlayerLocation;
import io.takaro.minecraft.core.model.PositionSample;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

import static org.junit.jupiter.api.Assertions.assertEquals;

class PositionPublisherTest {
    @Test
    void capturesEveryOnlinePlayerButNeverCloserThanTheDeclaredInterval() {
        AtomicLong now = new AtomicLong(1);
        PositionPublisher publisher = new PositionPublisher(now::get);
        var adapter = new TakaroConnectorTest.TestAdapter() {
            @Override public List<PlayerInfo> getPlayers() {
                return List.of(PlayerInfo.of("u1", "Steve", "", 0),
                        PlayerInfo.of("u2", "Alex", "", 0));
            }
            @Override public PlayerLocation getPlayerLocation(String gameId) {
                return new PlayerLocation(gameId.equals("u1") ? 1 : 2, 64, 3, "overworld");
            }
        };
        List<List<PositionSample>> batches = new ArrayList<>();
        EventEmitter emitter = new RecordingEmitter(batches);

        publisher.capture(adapter, emitter);
        now.addAndGet(TimeUnit.SECONDS.toNanos(2));
        publisher.capture(adapter, emitter);
        assertEquals(1, batches.size());
        now.addAndGet(TimeUnit.SECONDS.toNanos(1));
        publisher.capture(adapter, emitter);
        assertEquals(2, batches.size());
        assertEquals(List.of("u1", "u2"), batches.get(0).stream().map(PositionSample::gameId).toList());
        assertEquals(2, batches.get(1).get(1).position().x());
    }

    private record RecordingEmitter(List<List<PositionSample>> batches) implements EventEmitter {
        @Override public boolean acceptsPlayerPositions() { return true; }
        @Override public void emitPlayerPositions(List<PositionSample> positions) { batches.add(positions); }
        @Override public void emitPlayerConnected(PlayerInfo player) {}
        @Override public void emitPlayerDisconnected(String gameId, String playerName) {}
        @Override public void emitChatMessage(String gameId, String playerName, String channel, String message) {}
        @Override public void emitPlayerDeath(String gameId, String playerName, String attackerGameId, String attackerName,
                                              double x, double y, double z, String dimension) {}
        @Override public void emitEntityKilled(String gameId, String playerName, String entityCode, String weaponCode) {}
        @Override public void emitLog(String message) {}
    }
}
