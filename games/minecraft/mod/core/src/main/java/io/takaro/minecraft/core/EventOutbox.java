package io.takaro.minecraft.core;

import com.google.gson.JsonObject;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.UUID;

/**
 * Protocol-v1 event stream: every event gets the next {@code seq} of one {@code streamId} and
 * stays here until Takaro acknowledges it, so events raised while the connection is down are
 * resent after the next identify. One outbox lives as long as the server process, across
 * reconnects and config reloads; a restart starts a new stream.
 */
public final class EventOutbox {

    static final int CAPACITY = 5000;

    private final String streamId;
    private final Deque<JsonObject> pending = new ArrayDeque<>();
    private long nextSeq = 1;
    private long dropped;

    public EventOutbox() {
        this(UUID.randomUUID().toString().replace("-", ""));
    }

    EventOutbox(String streamId) {
        this.streamId = streamId;
    }

    public String streamId() {
        return streamId;
    }

    /** Queues an event and returns its gameEvent payload ({@code type}, {@code data}, {@code streamId}, {@code seq}). */
    public synchronized JsonObject add(String type, JsonObject data) {
        JsonObject payload = new JsonObject();
        payload.addProperty("type", type);
        payload.add("data", data);
        payload.addProperty("streamId", streamId);
        payload.addProperty("seq", nextSeq++);
        pending.addLast(payload);
        while (pending.size() > CAPACITY) {
            // Takaro only needs seq to grow, so a gap after a long outage is harmless.
            pending.removeFirst();
            dropped++;
        }
        return payload;
    }

    /** Forgets every event of this stream up to and including {@code upTo}. */
    public synchronized void ack(String ackStreamId, long upTo) {
        if (!streamId.equals(ackStreamId)) return;
        while (!pending.isEmpty() && pending.peekFirst().get("seq").getAsLong() <= upTo) {
            pending.removeFirst();
        }
    }

    /** The unacknowledged events, oldest first, to resend after identify. */
    public synchronized List<JsonObject> unacked() {
        return new ArrayList<>(pending);
    }

    public synchronized int size() {
        return pending.size();
    }

    /** Events dropped because the outbox was full. */
    public synchronized long dropped() {
        return dropped;
    }
}
