package dev.takaro.hytale.websocket;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class ReconnectBackoffTest {

    @Test
    void doublesFromThreeSecondsUpToOneMinute() {
        assertEquals(3_000, TakaroWebSocket.reconnectDelayMs(1));
        assertEquals(6_000, TakaroWebSocket.reconnectDelayMs(2));
        assertEquals(48_000, TakaroWebSocket.reconnectDelayMs(5));
        assertEquals(60_000, TakaroWebSocket.reconnectDelayMs(6));
    }

    @Test
    void neverOverflowsOnALongOutage() {
        // (int) Math.pow(2, n) * 3000 overflowed from attempt 21 on: the rig scheduled
        // reconnects -1271552 s and 1947802 s (22 days) away and never came back.
        for (int attempt = 1; attempt <= 10_000; attempt++) {
            int delay = TakaroWebSocket.reconnectDelayMs(attempt);
            assertTrue(delay >= 3_000 && delay <= 60_000, "attempt " + attempt + " gave " + delay);
        }
        assertEquals(60_000, TakaroWebSocket.reconnectDelayMs(Integer.MAX_VALUE));
    }
}
