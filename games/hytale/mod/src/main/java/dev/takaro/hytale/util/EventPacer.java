package dev.takaro.hytale.util;

/**
 * Token bucket for game events sent to Takaro.
 *
 * <p>Takaro rate-limits each server's events (100 per 5 s sustained, 600 per 30 s) and drops
 * the excess. A Hytale server writes hundreds of log lines while it boots, before the plugin
 * has identified; sent in one go after identify, part of them were dropped on every restart.
 * Every event leaves through this bucket, in seq order, so a backlog drains under the limit.
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar. Not thread-safe:
 * callers synchronise.
 */
public final class EventPacer {
    private final double capacity;
    private final double perMs;
    private double tokens;
    private long last;

    /**
     * @param perSecond sustained rate
     * @param burst     bucket size (also the start level)
     * @param nowMs     clock reading at construction
     */
    public EventPacer(double perSecond, int burst, long nowMs) {
        this.capacity = burst;
        this.perMs = perSecond / 1000.0;
        this.tokens = burst;
        this.last = nowMs;
    }

    /** Take one token if there is one. */
    public boolean tryAcquire(long nowMs) {
        refill(nowMs);
        if (tokens >= 1.0) {
            tokens -= 1.0;
            return true;
        }
        return false;
    }

    private void refill(long nowMs) {
        if (nowMs > last) {
            tokens = Math.min(capacity, tokens + (nowMs - last) * perMs);
            last = nowMs;
        }
    }
}
