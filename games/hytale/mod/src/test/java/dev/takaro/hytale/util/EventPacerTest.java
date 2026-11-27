package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class EventPacerTest {

    @Test
    void burstThenSustainedRateStaysUnderTakaroLimits() {
        long t = 0;
        EventPacer pacer = new EventPacer(14.0, 20, t);
        int sent = 0;
        // A boot backlog of 620 events offered every 250 ms for 30 s.
        int backlog = 620;
        int maxIn5s = 0;
        int[] perTick = new int[121];
        for (int tick = 0; tick <= 120; tick++) {
            t = tick * 250L;
            while (backlog > 0 && pacer.tryAcquire(t)) {
                backlog--;
                sent++;
                perTick[tick]++;
            }
        }
        for (int start = 0; start + 20 <= perTick.length; start++) {
            int sum = 0;
            for (int i = start; i < start + 20; i++) {
                sum += perTick[i];
            }
            maxIn5s = Math.max(maxIn5s, sum);
        }
        // Takaro: 100 per 5 s sustained, 600 per 30 s burst.
        assertTrue(maxIn5s <= 100, "max per 5 s was " + maxIn5s);
        assertTrue(sent <= 600, "sent in 30 s was " + sent);
        assertTrue(sent >= 350, "pacer too slow: " + sent);
    }

    @Test
    void emptyBucketRefillsOverTime() {
        EventPacer pacer = new EventPacer(10.0, 2, 0);
        assertTrue(pacer.tryAcquire(0));
        assertTrue(pacer.tryAcquire(0));
        assertFalse(pacer.tryAcquire(0));
        assertTrue(pacer.tryAcquire(100));
        assertFalse(pacer.tryAcquire(100));
        // Never above the burst size after a long idle period.
        assertTrue(pacer.tryAcquire(60_000));
        assertTrue(pacer.tryAcquire(60_000));
        assertFalse(pacer.tryAcquire(60_000));
    }
}
