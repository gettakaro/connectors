package io.takaro.zomboid.agent;

import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;

/**
 * A simple thread-safe token-bucket rate limiter.
 *
 * <p>Used to cap the firehose events ({@code entity-killed} on every zombie kill,
 * {@code log} on every server log line) so a horde or a chatty log cannot flood
 * the Takaro WebSocket. Capacity is the burst size; tokens refill continuously at
 * {@code refillPerSecond}. {@link #tryAcquire()} is lock-free and safe to call
 * from a game advice body (no allocation).
 *
 * <p>The clock is injectable ({@link LongSupplier} of nanoseconds) so the refill
 * behaviour can be unit-tested deterministically.
 */
public final class TokenBucket {

    private final long capacity;
    private final double tokensPerNano;
    private final LongSupplier clockNanos;

    // tokens are stored scaled by 1e6 in a long to keep the update CAS-able
    private static final long SCALE = 1_000_000L;
    private final AtomicLong scaledTokens;
    private final AtomicLong lastRefillNanos;

    public TokenBucket(long capacity, double refillPerSecond) {
        this(capacity, refillPerSecond, System::nanoTime);
    }

    public TokenBucket(long capacity, double refillPerSecond, LongSupplier clockNanos) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("capacity must be > 0");
        }
        if (refillPerSecond <= 0) {
            throw new IllegalArgumentException("refillPerSecond must be > 0");
        }
        this.capacity = capacity;
        this.tokensPerNano = refillPerSecond / 1_000_000_000.0;
        this.clockNanos = clockNanos;
        this.scaledTokens = new AtomicLong(capacity * SCALE);
        this.lastRefillNanos = new AtomicLong(clockNanos.getAsLong());
    }

    /**
     * Try to take one token. Returns {@code true} if a token was available (the
     * event is allowed through) or {@code false} if the bucket is empty (drop).
     */
    public boolean tryAcquire() {
        refill();
        while (true) {
            long current = scaledTokens.get();
            if (current < SCALE) {
                return false;
            }
            if (scaledTokens.compareAndSet(current, current - SCALE)) {
                return true;
            }
        }
    }

    private void refill() {
        long now = clockNanos.getAsLong();
        long last = lastRefillNanos.get();
        long elapsed = now - last;
        if (elapsed <= 0) {
            return;
        }
        if (!lastRefillNanos.compareAndSet(last, now)) {
            return; // another thread refilled; its update covers this window
        }
        long added = (long) (elapsed * tokensPerNano * SCALE);
        if (added <= 0) {
            // restore lastRefill so we do not lose sub-token time
            lastRefillNanos.set(last);
            return;
        }
        long max = capacity * SCALE;
        while (true) {
            long current = scaledTokens.get();
            long updated = Math.min(max, current + added);
            if (scaledTokens.compareAndSet(current, updated)) {
                return;
            }
        }
    }

    /** Current whole-token count (for tests / diagnostics). */
    public long availableTokens() {
        refill();
        return scaledTokens.get() / SCALE;
    }
}
