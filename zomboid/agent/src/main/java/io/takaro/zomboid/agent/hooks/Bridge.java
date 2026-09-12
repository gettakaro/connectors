package io.takaro.zomboid.agent.hooks;

import io.takaro.zomboid.agent.AgentLog;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The single static entry point that instrumented game code calls into.
 *
 * <p>Every advice body is exactly one call to a method here, and every method
 * here catches {@link Throwable}: an exception must never escape into a game
 * code path. This class deliberately has no static initialiser that touches
 * game classes, so loading it can never fail mid-tick.
 */
public final class Bridge {

    private static final long TICK_LOG_INTERVAL = 600L;
    private static final AtomicLong TICKS = new AtomicLong();

    private Bridge() {
    }

    /** Called from {@code zombie.network.RCONServer.update()} — main thread, once per tick. */
    public static void tick() {
        try {
            long n = TICKS.incrementAndGet();
            if (n % TICK_LOG_INTERVAL == 0L) {
                AgentLog.log("tick count=" + n + " thread=" + Thread.currentThread().getName());
            }
        } catch (Throwable t) {
            // never propagate into the game main loop
        }
    }

    /** Called from {@code zombie.network.GameServer.receivePlayerConnect(..)} on exit. */
    public static void join(String username) {
        try {
            AgentLog.log("join username=" + username
                    + " thread=" + Thread.currentThread().getName()
                    + " tick=" + TICKS.get());
        } catch (Throwable t) {
            // never propagate into the game networking path
        }
    }
}
