package io.takaro.zomboid.core;

import java.util.function.LongSupplier;
import java.util.random.RandomGenerator;

/**
 * When to reconnect after the socket closed. A connection that was identified and stayed up for
 * {@link #STABLE_MS} reconnects almost at once ({@link #FAST_MIN_MS} plus up to
 * {@link #FAST_JITTER_MS} of jitter), so a cut on the path to Takaro is over before Takaro's
 * reachability check notices it. Only consecutive failures back off: the base delay doubles (by the
 * configured multiplier) per failure, capped at the configured maximum, and each delay is jittered
 * down to half its value so many servers never retry in step. The streak resets only once a
 * connection has been identified and stable, never because a socket merely opened.
 *
 * <p>Not thread safe on its own; the client calls it from its socket and reconnect threads one at
 * a time (a close always follows the identify of the same connection).
 */
public final class ReconnectPolicy {

    /** How long an identified connection must stay up before its drop counts as "was stable". */
    public static final long STABLE_MS = 10_000;
    /** Shortest first retry after a stable connection dropped. */
    public static final long FAST_MIN_MS = 500;
    /** Random spread added to {@link #FAST_MIN_MS}: the first retry lands in 0.5-1.5 s. */
    public static final long FAST_JITTER_MS = 1_000;

    private final TakaroConfig config;
    private final LongSupplier clockMs;
    private final RandomGenerator random;

    private int failureStreak = 0;
    private long identifiedAtMs = -1;

    public ReconnectPolicy(TakaroConfig config) {
        this(config, () -> System.nanoTime() / 1_000_000L, RandomGenerator.getDefault());
    }

    ReconnectPolicy(TakaroConfig config, LongSupplier clockMs, RandomGenerator random) {
        this.config = config;
        this.clockMs = clockMs;
        this.random = random;
    }

    /** The connection was identified; it counts as stable once it has lasted {@link #STABLE_MS}. */
    public void onIdentified() {
        identifiedAtMs = clockMs.getAsLong();
    }

    /** One decision per close: the delay before the next attempt and why. */
    public record Delay(long millis, boolean fast, int attempt) {
        public String describe() {
            String seconds = String.format(java.util.Locale.ROOT, "%.1fs", millis / 1000.0);
            return fast
                    ? seconds + " (fast retry, the connection was stable)"
                    : seconds + " (attempt " + attempt + ", backing off)";
        }
    }

    /** The socket closed (or a connect attempt failed): returns the delay before the next attempt. */
    public Delay nextDelay() {
        boolean wasStable = identifiedAtMs >= 0 && clockMs.getAsLong() - identifiedAtMs >= STABLE_MS;
        identifiedAtMs = -1;
        if (wasStable) {
            failureStreak = 0;
            return new Delay(FAST_MIN_MS + random.nextLong(FAST_JITTER_MS + 1), true, 1);
        }
        failureStreak++;
        return new Delay(jitter(backoffCeiling(failureStreak)), false, failureStreak);
    }

    /** The un-jittered delay for the n-th consecutive failure (1-based), within [base, cap]. */
    long backoffCeiling(int failures) {
        long base = Math.max(1, config.getReconnectDelay());
        long cap = Math.max(base, config.getMaxReconnectDelay());
        double multiplier = Math.max(1.0, config.getBackoffMultiplier());
        double delay = base * Math.pow(multiplier, Math.max(0, failures - 1));
        return delay >= cap ? cap : (long) delay;
    }

    /** "Equal jitter": somewhere in [ceiling/2, ceiling], so never above the cap. */
    private long jitter(long ceiling) {
        long half = ceiling / 2;
        return half + random.nextLong(ceiling - half + 1);
    }

    int failureStreak() {
        return failureStreak;
    }
}
