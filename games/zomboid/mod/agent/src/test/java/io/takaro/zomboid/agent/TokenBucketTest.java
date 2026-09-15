package io.takaro.zomboid.agent;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.atomic.AtomicLong;

import org.junit.jupiter.api.Test;

class TokenBucketTest {

    /** A manual nanosecond clock so refill behaviour is deterministic. */
    private static final class FakeClock {
        final AtomicLong now = new AtomicLong(0);

        long get() {
            return now.get();
        }

        void advanceMillis(long ms) {
            now.addAndGet(ms * 1_000_000L);
        }
    }

    @Test
    void allowsUpToBurstThenDrops() {
        FakeClock clock = new FakeClock();
        TokenBucket bucket = new TokenBucket(50, 20.0, clock::get);

        int allowed = 0;
        for (int i = 0; i < 60; i++) {
            if (bucket.tryAcquire()) {
                allowed++;
            }
        }
        // capacity 50 is the burst; the 10 extra within the same instant are dropped
        assertEquals(50, allowed, "burst should be capped at capacity");
        assertFalse(bucket.tryAcquire(), "bucket is empty after the burst");
    }

    @Test
    void refillsAtConfiguredRate() {
        FakeClock clock = new FakeClock();
        TokenBucket bucket = new TokenBucket(50, 20.0, clock::get);

        // drain the whole burst
        for (int i = 0; i < 50; i++) {
            assertTrue(bucket.tryAcquire());
        }
        assertFalse(bucket.tryAcquire());

        // after 1 second at 20/s, ~20 tokens should be available again
        clock.advanceMillis(1000);
        int refilled = 0;
        for (int i = 0; i < 30; i++) {
            if (bucket.tryAcquire()) {
                refilled++;
            }
        }
        assertEquals(20, refilled, "should refill 20 tokens after 1s at 20/s");
    }

    @Test
    void refillIsCappedAtCapacity() {
        FakeClock clock = new FakeClock();
        TokenBucket bucket = new TokenBucket(50, 20.0, clock::get);

        // drain, then wait a long time — tokens must not exceed capacity
        for (int i = 0; i < 50; i++) {
            bucket.tryAcquire();
        }
        clock.advanceMillis(60_000); // 60s would be 1200 tokens uncapped
        assertEquals(50, bucket.availableTokens(), "tokens cap at capacity");
    }

    @Test
    void halfSecondRefillsHalfRate() {
        FakeClock clock = new FakeClock();
        TokenBucket bucket = new TokenBucket(50, 20.0, clock::get);
        for (int i = 0; i < 50; i++) {
            bucket.tryAcquire();
        }
        clock.advanceMillis(500); // 0.5s at 20/s => 10 tokens
        assertEquals(10, bucket.availableTokens());
    }
}
