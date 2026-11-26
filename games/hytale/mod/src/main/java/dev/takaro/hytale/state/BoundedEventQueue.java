package dev.takaro.hytale.state;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;

/**
 * Holds game events that cannot be sent yet.
 *
 * <p>The connector used to drop every event produced while the socket was down or before
 * {@code identifyResponse} arrived: a chat message, death or connect during a Takaro outage was
 * lost permanently. Events are now parked here and flushed once the connection is
 * identified.
 *
 * <p>The queue is bounded and drops the OLDEST event when full, so a long outage costs the
 * start of the backlog rather than unbounded memory, and the number dropped is counted so it
 * can be logged honestly rather than hidden.
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar.
 */
public class BoundedEventQueue<T> {
    private final int capacity;
    private final Deque<T> queue;
    private long dropped;

    public BoundedEventQueue(int capacity) {
        this.capacity = Math.max(1, capacity);
        this.queue = new ArrayDeque<>(Math.min(this.capacity, 256));
    }

    /**
     * Park an event.
     *
     * @return true if it fitted, false if the oldest event had to be dropped to make room
     */
    public synchronized boolean offer(T event) {
        if (event == null) {
            return true;
        }
        boolean fitted = true;
        while (queue.size() >= capacity) {
            queue.pollFirst();
            dropped++;
            fitted = false;
        }
        queue.addLast(event);
        return fitted;
    }

    /** Take everything currently queued, leaving the queue empty. */
    public synchronized List<T> drain() {
        List<T> out = new ArrayList<>(queue);
        queue.clear();
        return out;
    }

    public synchronized int size() {
        return queue.size();
    }

    public synchronized boolean isEmpty() {
        return queue.isEmpty();
    }

    public int capacity() {
        return capacity;
    }

    /** Total number of events discarded because the queue was full, since startup. */
    public synchronized long droppedCount() {
        return dropped;
    }

    /** Drop everything without sending, e.g. on shutdown. */
    public synchronized void clear() {
        queue.clear();
    }
}
