package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.EventEmitter;
import io.takaro.zomboid.core.model.*;
import org.junit.jupiter.api.Test;
import java.lang.reflect.Proxy;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import static org.junit.jupiter.api.Assertions.*;

class PositionPublisherTest {
    private final AtomicLong now = new AtomicLong(TimeUnit.SECONDS.toNanos(100));
    private final List<PlayerInfo> roster = new ArrayList<>(List.of(player("alive"), player("loading")));
    private final Map<String, PlayerLocation> locations = new HashMap<>(Map.of("alive", new PlayerLocation(1,2,0,"knox-county")));
    private final List<Boolean> availability = new ArrayList<>();
    private final List<List<PlayerPositionSample>> batches = new ArrayList<>();
    private boolean available = true;
    private int reads;
    private final EventEmitter emitter = (EventEmitter) Proxy.newProxyInstance(EventEmitter.class.getClassLoader(),
            new Class<?>[]{EventEmitter.class}, (proxy, method, args) -> {
                if (method.getName().equals("canPushPositions")) return available;
                if (method.getName().equals("setPositionPushAvailable")) {
                    available = (boolean)args[0]; availability.add(available);
                }
                if (method.getName().equals("emitPlayerPositions")) {
                    @SuppressWarnings("unchecked") var batch = (List<PlayerPositionSample>)args[0];
                    batches.add(batch);
                    assertEquals(now.get(), args[1]);
                }
                return null;
            });
    private final PositionPublisher publisher = new PositionPublisher(() -> { reads++; return roster; }, locations::get, now::get);

    @Test
    void loadingOrDeadPlayerWithdrawsPushUntilCompleteWithBoundedRetries() {
        publisher.tick(emitter);
        assertFalse(available);
        assertTrue(batches.isEmpty(), "never send a partial roster");
        for (int i=0;i<100;i++) { now.addAndGet(TimeUnit.MILLISECONDS.toNanos(29)); publisher.tick(emitter); }
        assertEquals(1, reads, "unreadable players are retried at most every three seconds");
        now.addAndGet(TimeUnit.MILLISECONDS.toNanos(100));
        publisher.tick(emitter);
        assertEquals(2, reads);
        assertFalse(available);
        locations.put("loading", new PlayerLocation(4,5,0,"knox-county"));
        now.addAndGet(TimeUnit.SECONDS.toNanos(3));
        publisher.tick(emitter);
        assertTrue(available);
        assertEquals(1, batches.size());
        assertEquals(List.of("alive", "loading"), batches.get(0).stream().map(PlayerPositionSample::gameId).toList());
        assertEquals(List.of(false,false,true), availability);
    }

    @Test
    void leavingUnreadablePlayerRestoresCompleteRosterAndInvalidCoordinatesWithdrawAgain() {
        publisher.tick(emitter);
        roster.remove(1);
        now.addAndGet(TimeUnit.SECONDS.toNanos(3));
        publisher.tick(emitter);
        assertTrue(available);
        assertEquals(1,batches.get(0).size());
        locations.put("alive",new PlayerLocation(Double.NaN,1,0,null));
        now.addAndGet(TimeUnit.SECONDS.toNanos(3));
        publisher.tick(emitter);
        assertFalse(available);
        assertEquals(1,batches.size());
        roster.clear();
        now.addAndGet(TimeUnit.SECONDS.toNanos(3));
        publisher.tick(emitter);
        assertTrue(available);
        assertTrue(batches.get(1).isEmpty(), "empty complete roster restores push");
    }

    private static PlayerInfo player(String id) { return new PlayerInfo(id,id,null,null,null,null,null,0); }
}
