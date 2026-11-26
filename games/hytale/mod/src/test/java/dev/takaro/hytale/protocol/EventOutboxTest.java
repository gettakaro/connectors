package dev.takaro.hytale.protocol;

import org.junit.jupiter.api.Test;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class EventOutboxTest {

    private static Map<String, Object> data(String msg) {
        Map<String, Object> d = new LinkedHashMap<>();
        d.put("msg", msg);
        return d;
    }

    private static List<Long> seqs(List<EventOutbox.Pending> events) {
        return events.stream().map(EventOutbox.Pending::seq).toList();
    }

    @Test
    void numbersEventsFromOneOnOneStream() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        EventOutbox.Pending a = outbox.add("log", data("a"));
        EventOutbox.Pending b = outbox.add("chat-message", data("b"));
        assertEquals(1, a.seq());
        assertEquals(2, b.seq());
        assertEquals("s1", a.payload().get("streamId"));
        assertEquals(1L, a.payload().get("seq"));
        assertEquals("log", a.payload().get("type"));
        assertEquals(data("a"), a.payload().get("data"));
    }

    @Test
    void eachProcessGetsItsOwnRandomStream() {
        assertNotEquals(new EventOutbox(5).streamId(), new EventOutbox(5).streamId());
    }

    @Test
    void cumulativeAckRemovesEverythingUpToIt() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        for (int i = 0; i < 5; i++) {
            outbox.add("log", data("e" + i));
        }
        assertEquals(3, outbox.acknowledge("s1", 3));
        assertEquals(List.of(4L, 5L), seqs(outbox.unacknowledged()));
        // An ack for an older seq changes nothing.
        assertEquals(0, outbox.acknowledge("s1", 2));
        assertEquals(2, outbox.size());
    }

    @Test
    void ackForAnotherStreamIsIgnored() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        outbox.add("log", data("a"));
        assertEquals(0, outbox.acknowledge("other", 100));
        assertEquals(1, outbox.size());
    }

    @Test
    void unacknowledgedAreResentOldestFirstAndKeptUntilAcked() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        outbox.add("log", data("a"));
        outbox.add("log", data("b"));
        outbox.add("log", data("c"));
        assertEquals(List.of(1L, 2L, 3L), seqs(outbox.unacknowledged()));
        // A second identify resends the same events again.
        assertEquals(List.of(1L, 2L, 3L), seqs(outbox.unacknowledged()));
    }

    @Test
    void overflowDropsTheOldestAndCountsIt() {
        EventOutbox outbox = new EventOutbox("s1", 3);
        for (int i = 0; i < 5; i++) {
            outbox.add("log", data("e" + i));
        }
        assertEquals(List.of(3L, 4L, 5L), seqs(outbox.unacknowledged()));
        assertEquals(2, outbox.dropped());
        // seq keeps growing after drops: Takaro accepts a gap.
        assertEquals(6, outbox.add("log", data("x")).seq());
    }

    @Test
    void capacityIsAtLeastOne() {
        EventOutbox outbox = new EventOutbox("s1", 0);
        outbox.add("log", data("a"));
        outbox.add("log", data("b"));
        assertEquals(List.of(2L), seqs(outbox.unacknowledged()));
    }

    @Test
    void drainEmptiesForAProtocolZeroFlush() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        outbox.add("log", data("a"));
        outbox.add("log", data("b"));
        assertEquals(List.of(1L, 2L), seqs(outbox.drain()));
        assertEquals(0, outbox.size());
    }

    @Test
    void protocolZeroShapeHasNoStreamFieldsAndNoDimension() {
        EventOutbox outbox = new EventOutbox("s1", 10);
        Map<String, Object> position = new LinkedHashMap<>();
        position.put("x", 1.0);
        position.put("y", 2.0);
        position.put("z", 3.0);
        position.put("dimension", "default");
        Map<String, Object> d = new LinkedHashMap<>();
        d.put("position", position);
        EventOutbox.Pending event = outbox.add("player-death", d);

        Map<String, Object> v0 = EventOutbox.toV0Payload(event.payload());
        assertEquals(List.of("type", "data"), List.copyOf(v0.keySet()));
        assertEquals("player-death", v0.get("type"));
        @SuppressWarnings("unchecked")
        Map<String, Object> v0Position = (Map<String, Object>) ((Map<String, Object>) v0.get("data")).get("position");
        assertEquals(Map.of("x", 1.0, "y", 2.0, "z", 3.0), v0Position);
        // The buffered v1 event is untouched.
        assertEquals("default", position.get("dimension"));
        assertEquals("s1", event.payload().get("streamId"));
    }

    @Test
    void protocolZeroShapeKeepsEventsWithoutPositionAsTheyAre() {
        Map<String, Object> d = data("hello");
        Map<String, Object> v0 = EventOutbox.toV0Payload(new EventOutbox("s1", 1).add("chat-message", d).payload());
        assertSame(d, v0.get("data"));
    }
}
