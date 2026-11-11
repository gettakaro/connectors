package io.takaro.minecraft.core;

/**
 * When to reconnect to Takaro.
 *
 * A connection that was identified and stayed up for {@link #STABLE_MILLIS} resets the failure
 * streak; when it drops, the next attempt starts {@link #FAST_MIN_MILLIS} to
 * {@link #FAST_MIN_MILLIS} + {@link #FAST_SPREAD_MILLIS} later, so it is identified again within
 * about 2 s. Only consecutive failures back off: the configured base delay times the multiplier
 * per failure, each wait randomly between half and all of its step, never past
 * {@link #MAX_DELAY_MILLIS} (or a lower configured max_delay).
 */
final class ReconnectPolicy {
    static final long FAST_MIN_MILLIS = 200;
    static final long FAST_SPREAD_MILLIS = 500;
    static final long STABLE_MILLIS = 10_000;
    static final long MAX_DELAY_MILLIS = 60_000;
    static final long PING_INTERVAL_MILLIS = 15_000;
    /** Three missed pongs. */
    static final long INBOUND_TIMEOUT_MILLIS = 45_000;
    /** Connect plus identify. */
    static final long HANDSHAKE_TIMEOUT_MILLIS = 20_000;

    private final long stableMillis;
    private int failures;

    ReconnectPolicy() {
        this(STABLE_MILLIS);
    }

    /** Tests shorten how long a session must stay up to count as stable. */
    ReconnectPolicy(long stableMillis) {
        this.stableMillis = stableMillis;
    }

    long stableMillis() {
        return stableMillis;
    }

    /**
     * The wait before the next attempt after {@code failures} attempts in a row that did not reach
     * a stable connection. {@code random01} is in [0, 1).
     */
    static long delayMillis(int failures, long baseMillis, double multiplier, long maxMillis, double random01) {
        double r = Math.min(Math.max(random01, 0), 0.999999);
        if (failures <= 0) return FAST_MIN_MILLIS + (long) (FAST_SPREAD_MILLIS * r);
        long cap = Math.min(MAX_DELAY_MILLIS, maxMillis > 0 ? maxMillis : MAX_DELAY_MILLIS);
        double m = multiplier > 1 ? multiplier : 2.0;
        double step = Math.max(1, baseMillis) * Math.pow(m, Math.min(failures - 1, 64));
        long ceiling = (long) Math.min(cap, step);
        return ceiling / 2 + (long) (ceiling / 2.0 * r);
    }

    /**
     * A connection ended or never came up. {@code identifiedForMillis} is how long it had been
     * identified, or negative if it never was. Returns the delay before the next attempt.
     */
    synchronized long onConnectionEnded(long identifiedForMillis, long baseMillis, double multiplier,
                                        long maxMillis, double random01) {
        if (identifiedForMillis >= stableMillis) failures = 0;
        long delay = delayMillis(failures, baseMillis, multiplier, maxMillis, random01);
        failures++;
        return delay;
    }

    synchronized int failures() {
        return failures;
    }

    synchronized void reset() {
        failures = 0;
    }
}
