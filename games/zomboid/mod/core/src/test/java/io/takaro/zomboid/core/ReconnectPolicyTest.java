package io.takaro.zomboid.core;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.HashSet;
import java.util.Random;
import java.util.Set;

import static org.junit.jupiter.api.Assertions.*;

/** The reconnect delay policy (gettakaro/connectors#858): fast after a stable session, backoff only on a streak. */
class ReconnectPolicyTest {

    private long now;
    private TakaroConfig config;
    private ReconnectPolicy policy;

    @BeforeEach
    void setUp() {
        now = 1_000_000;
        config = new TakaroConfig();
        policy = new ReconnectPolicy(config, () -> now, new Random(42));
    }

    private void identifyAndStay(long ms) {
        policy.onIdentified();
        now += ms;
    }

    @Test
    void firstRetryAfterAStableSessionIsUnderOneSecond() {
        for (int i = 0; i < 500; i++) {
            identifyAndStay(ReconnectPolicy.STABLE_MS);
            ReconnectPolicy.Delay d = policy.nextDelay();
            assertTrue(d.fast(), "stable drop must take the fast retry");
            assertTrue(d.millis() >= 200 && d.millis() <= 900, "fast retry out of range: " + d.millis());
            assertEquals(0, policy.failureStreak());
        }
    }

    @Test
    void fastRetryIsJittered() {
        Set<Long> seen = new HashSet<>();
        for (int i = 0; i < 50; i++) {
            identifyAndStay(60_000);
            seen.add(policy.nextDelay().millis());
        }
        assertTrue(seen.size() > 10, "fast retries should spread out, got " + seen);
    }

    @Test
    void connectionThatDiesBeforeItIsStableBacksOff() {
        identifyAndStay(ReconnectPolicy.STABLE_MS - 1);
        ReconnectPolicy.Delay d = policy.nextDelay();
        assertFalse(d.fast());
        assertEquals(1, d.attempt());
        assertTrue(d.millis() >= 1000 && d.millis() <= 2000, "first backoff out of range: " + d.millis());
    }

    @Test
    void consecutiveFailuresDoubleWithJitterUpToTheSixtySecondCap() {
        long[] ceilings = {2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
        for (int i = 0; i < ceilings.length; i++) {
            ReconnectPolicy.Delay d = policy.nextDelay(); // never identified: a failed attempt
            assertFalse(d.fast());
            assertEquals(i + 1, d.attempt());
            assertEquals(ceilings[i], policy.backoffCeiling(i + 1));
            assertTrue(d.millis() >= ceilings[i] / 2 && d.millis() <= ceilings[i],
                    "attempt " + (i + 1) + ": " + d.millis() + " not in [" + ceilings[i] / 2 + ", " + ceilings[i] + "]");
        }
        for (int i = 0; i < 1000; i++) {
            assertTrue(policy.nextDelay().millis() <= 60_000, "cap exceeded");
        }
    }

    @Test
    void streakResetsOnlyAfterAStableIdentifiedConnection() {
        policy.nextDelay();
        policy.nextDelay();
        policy.nextDelay();
        assertEquals(3, policy.failureStreak());
        identifyAndStay(2_000); // identified but dropped quickly: still failing
        assertEquals(4, policy.nextDelay().attempt());
        identifyAndStay(ReconnectPolicy.STABLE_MS);
        assertTrue(policy.nextDelay().fast());
        assertEquals(0, policy.failureStreak());
        // the fast retry itself fails: the streak starts over at the base delay
        ReconnectPolicy.Delay d = policy.nextDelay();
        assertEquals(1, d.attempt());
        assertTrue(d.millis() <= 2000);
    }

    @Test
    void identifyIsCountedPerConnection() {
        identifyAndStay(ReconnectPolicy.STABLE_MS);
        assertTrue(policy.nextDelay().fast());
        now += 60_000; // a later close with no identify in between (connect failure)
        assertFalse(policy.nextDelay().fast());
    }

    @Test
    void operatorOverridesStillShapeTheBackoff() {
        config.setReconnectDelay(10_000);
        config.setMaxReconnectDelay(30_000);
        config.setBackoffMultiplier(1.5);
        assertEquals(10_000, policy.backoffCeiling(1));
        assertEquals(15_000, policy.backoffCeiling(2));
        assertEquals(22_500, policy.backoffCeiling(3));
        assertEquals(30_000, policy.backoffCeiling(4));
        // an override never slows the first retry after a stable connection
        identifyAndStay(ReconnectPolicy.STABLE_MS);
        assertTrue(policy.nextDelay().millis() <= 900);
    }

    @Test
    void clientDetectsADeadLinkWithinThirtySeconds() throws Exception {
        TakaroWebSocketClient client = new TakaroWebSocketClient(
                new java.net.URI("ws://localhost:9999"), noopAdapter(), config);
        // Java-WebSocket pings every timeout and closes after 1.5x without a pong: worst case 2.5x.
        assertEquals(TakaroWebSocketClient.CONNECTION_LOST_TIMEOUT_SECONDS, client.getConnectionLostTimeout());
        assertTrue(client.getConnectionLostTimeout() * 2.5 <= 30, "a dead link must be noticed within 30 s");
    }

    private static GameAdapter noopAdapter() {
        return (GameAdapter) java.lang.reflect.Proxy.newProxyInstance(GameAdapter.class.getClassLoader(),
                new Class<?>[]{GameAdapter.class},
                (proxy, method, args) -> method.isDefault()
                        ? java.lang.reflect.InvocationHandler.invokeDefault(proxy, method, args) : null);
    }
}
