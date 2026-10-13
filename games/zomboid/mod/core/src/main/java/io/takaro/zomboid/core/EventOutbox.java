package io.takaro.zomboid.core;

import com.google.gson.JsonObject;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.UUID;

/**
 * Protocol v1 event delivery: every event gets the next {@code seq} of one stream and waits
 * here until Takaro's cumulative {@code eventAck} covers it, so events produced while the
 * socket is down are resent after the next identify instead of being lost.
 *
 * <p>The stream id is random per outbox, and the outbox lives as long as the process (the
 * {@link TakaroConnector} owns it), so a reconnect keeps the stream and a restart starts a
 * new one, as the protocol requires. Bounded: on overflow the oldest event is dropped and
 * Takaro sees a seq gap.
 */
public final class EventOutbox {

    public static final int DEFAULT_CAPACITY = 1000;

    /** One buffered event: its seq and the full {@code gameEvent} payload. */
    public record Pending(long seq, JsonObject payload) {
    }

    private final String streamId;
    private final int capacity;
    private final Deque<Pending> pending = new ArrayDeque<>();
    private long nextSeq = 1;
    private long dropped;

    public EventOutbox() {
        this(UUID.randomUUID().toString(), DEFAULT_CAPACITY);
    }

    public EventOutbox(String streamId, int capacity) {
        if (capacity < 1) {
            throw new IllegalArgumentException("capacity must be at least 1");
        }
        this.streamId = streamId;
        this.capacity = capacity;
    }

    public String streamId() {
        return streamId;
    }

    /** Buffers an event and returns it with its seq, streamId and seq already in the payload. */
    public synchronized Pending add(String type, JsonObject data) {
        JsonObject payload = new JsonObject();
        payload.addProperty("type", type);
        payload.add("data", data);
        payload.addProperty("streamId", streamId);
        long seq = nextSeq++;
        payload.addProperty("seq", seq);
        if (pending.size() >= capacity) {
            pending.removeFirst();
            dropped++;
        }
        Pending event = new Pending(seq, payload);
        pending.addLast(event);
        return event;
    }

    /** Every unacknowledged event, oldest first. */
    public synchronized List<Pending> unacknowledged() {
        return new ArrayList<>(pending);
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

    /** Forgets every buffered event (a protocol 0 connection has no acks). */
    public synchronized List<Pending> drain() {
        List<Pending> all = new ArrayList<>(pending);
        pending.clear();
        return all;
    }

    public synchronized int size() {
        return pending.size();
    }

    public synchronized long dropped() {
        return dropped;
    }
}
