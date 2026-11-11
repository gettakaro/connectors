package io.takaro.minecraft.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class ReconnectPolicyTest {

    @Test
    void stableSessionRetriesWithinAFewHundredMillis() {
        ReconnectPolicy p = new ReconnectPolicy();
        p.onConnectionEnded(-1, 1000, 2.0, 60_000, 0.5); // a failed first connect
        long d = p.onConnectionEnded(ReconnectPolicy.STABLE_MILLIS, 1000, 2.0, 60_000, 0.999);
        assertTrue(d >= 200 && d <= 700, "fast retry was " + d);
        assertEquals(1, p.failures());
    }

    @Test
    void oldFiveSecondDelayDoesNotSlowTheFastRetry() {
        assertTrue(ReconnectPolicy.delayMillis(0, 5000, 1.5, 300_000, 0.999) <= 700);
    }

    @Test
    void consecutiveFailuresBackOffWithEqualJitter() {
        assertEquals(500, ReconnectPolicy.delayMillis(1, 1000, 2.0, 60_000, 0));
        assertEquals(1000, ReconnectPolicy.delayMillis(2, 1000, 2.0, 60_000, 0));
        long high = ReconnectPolicy.delayMillis(3, 1000, 2.0, 60_000, 0.999999);
        assertTrue(high > 3990 && high <= 4000, "step 3 was " + high);
    }

    @Test
    void neverWaitsLongerThanSixtySecondsEvenWithTheOldMaxDelay() {
        for (int f = 1; f < 200; f++) {
            assertTrue(ReconnectPolicy.delayMillis(f, 5000, 1.5, 300_000, 0.999999) <= 60_000);
        }
        assertTrue(ReconnectPolicy.delayMillis(50, 1000, 2.0, 60_000, 0) >= 30_000);
    }

    @Test
    void lowerConfiguredMaxDelayIsHonoured() {
        assertTrue(ReconnectPolicy.delayMillis(20, 1000, 2.0, 10_000, 0.999999) <= 10_000);
    }

    @Test
    void shortSessionDoesNotResetTheStreak() {
        ReconnectPolicy p = new ReconnectPolicy();
        p.onConnectionEnded(-1, 1000, 2.0, 60_000, 0);
        p.onConnectionEnded(ReconnectPolicy.STABLE_MILLIS - 1, 1000, 2.0, 60_000, 0);
        assertEquals(2, p.failures());
        assertEquals(1000, p.onConnectionEnded(500, 1000, 2.0, 60_000, 0));
    }
}
