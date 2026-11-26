package dev.takaro.hytale.protocol;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Protocol v1 event delivery: every event gets the next {@code seq} of one stream and waits
 * here until Takaro's cumulative {@code eventAck} covers it, so events produced while the
 * socket is down, or before {@code identifyResponse}, are resent after the next identify
 * instead of being lost.
 *
 * <p>One outbox lives as long as the server process, across reconnects: a reconnect keeps the
 * stream and a restart starts a new one, as the protocol requires. Bounded: on overflow the
 * oldest event is dropped and Takaro sees a seq gap, which it accepts (it only needs seq to grow).
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar.
 */
public final class EventOutbox {

    /** One buffered event: its seq and the full v1 {@code gameEvent} payload. */
    public record Pending(long seq, Map<String, Object> payload) {
    }

    private final String streamId;
    private final int capacity;
    private final Deque<Pending> pending = new ArrayDeque<>();
    private long nextSeq = 1;
    private long dropped;

    public EventOutbox(int capacity) {
        this(UUID.randomUUID().toString(), capacity);
    }

    public EventOutbox(String streamId, int capacity) {
        this.streamId = streamId;
        this.capacity = Math.max(1, capacity);
    }

    public String streamId() {
        return streamId;
    }

    public int capacity() {
        return capacity;
    }

    /** Buffers an event; the returned payload already carries {@code streamId} and {@code seq}. */
    public synchronized Pending add(String type, Map<String, Object> data) {
        Map<String, Object> payload = new LinkedHashMap<>();
        payload.put("type", type);
        payload.put("data", data);
        payload.put("streamId", streamId);
        long seq = nextSeq++;
        payload.put("seq", seq);
        while (pending.size() >= capacity) {
            pending.removeFirst();
            dropped++;
        }
        Pending event = new Pending(seq, payload);
        pending.addLast(event);
        return event;
    }

    /** Drops every event of this stream with seq at or below {@code upTo}; returns how many. */
    public synchronized int acknowledge(String ackStreamId, long upTo) {
        if (!streamId.equals(ackStreamId)) {
            return 0;
        }
        int removed = 0;
        while (!pending.isEmpty() && pending.peekFirst().seq() <= upTo) {
            pending.removeFirst();
            removed++;
        }
        return removed;
    }

    /** Every unacknowledged event, oldest first, to resend after identify. */
    public synchronized List<Pending> unacknowledged() {
        return new ArrayList<>(pending);
    }

    /** Up to {@code max} unacknowledged events with seq above {@code afterSeq}, oldest first. */
    public synchronized List<Pending> after(long afterSeq, int max) {
        List<Pending> out = new ArrayList<>(Math.min(max, pending.size()));
        for (Pending event : pending) {
            if (out.size() >= max) {
                break;
            }
            if (event.seq() > afterSeq) {
                out.add(event);
            }
        }
        return out;
    }

    /** Takes every buffered event: a protocol 0 connection has no acks, so they are sent once. */
    public synchronized List<Pending> drain() {
        List<Pending> all = new ArrayList<>(pending);
        pending.clear();
        return all;
    }

    public synchronized int size() {
        return pending.size();
    }

    /** Events dropped because the outbox was full, since startup. */
    public synchronized long dropped() {
        return dropped;
    }

    public synchronized void clear() {
        pending.clear();
    }

    /**
     * The protocol 0 {@code gameEvent} payload for a buffered event: only {@code type} and
     * {@code data}, and no {@code dimension} in a position, since a v0 Takaro knows no map
     * dimensions.
     */
    @SuppressWarnings("unchecked")
    public static Map<String, Object> toV0Payload(Map<String, Object> v1Payload) {
        Map<String, Object> plain = new LinkedHashMap<>();
        plain.put("type", v1Payload.get("type"));
        Object data = v1Payload.get("data");
        if (data instanceof Map<?, ?> map && map.get("position") instanceof Map<?, ?> position
                && position.containsKey("dimension")) {
            Map<String, Object> dataCopy = new LinkedHashMap<>((Map<String, Object>) map);
            Map<String, Object> positionCopy = new LinkedHashMap<>((Map<String, Object>) position);
            positionCopy.remove("dimension");
            dataCopy.put("position", positionCopy);
            data = dataCopy;
        }
        plain.put("data", data);
        return plain;
    }
}
