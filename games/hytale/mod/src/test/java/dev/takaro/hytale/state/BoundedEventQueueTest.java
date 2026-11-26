package dev.takaro.hytale.state;

import org.junit.jupiter.api.Test;

import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class BoundedEventQueueTest {

    @Test
    void holdsEventsAndFlushesThemInOrder() {
        BoundedEventQueue<String> q = new BoundedEventQueue<>(10);
        q.offer("a");
        q.offer("b");
        q.offer("c");
        assertEquals(3, q.size());

        List<String> drained = q.drain();
        assertEquals(List.of("a", "b", "c"), drained);
        assertTrue(q.isEmpty());
        assertEquals(0, q.droppedCount());
    }

    @Test
    void dropsTheOldestWhenFullAndCountsTheDrops() {
        BoundedEventQueue<String> q = new BoundedEventQueue<>(3);
        assertTrue(q.offer("1"));
        assertTrue(q.offer("2"));
        assertTrue(q.offer("3"));
        assertFalse(q.offer("4"));   // reports that something had to go
        assertFalse(q.offer("5"));

        assertEquals(3, q.size());
        assertEquals(2, q.droppedCount());
        assertEquals(List.of("3", "4", "5"), q.drain());
    }

    @Test
    void droppedCountSurvivesDraining() {
        BoundedEventQueue<String> q = new BoundedEventQueue<>(1);
        q.offer("a");
        q.offer("b");
        q.drain();
        assertEquals(1, q.droppedCount());
    }

    @Test
    void nullEventsAreIgnored() {
        BoundedEventQueue<String> q = new BoundedEventQueue<>(2);
        assertTrue(q.offer(null));
        assertEquals(0, q.size());
    }

    @Test
    void capacityIsAtLeastOne() {
        assertEquals(1, new BoundedEventQueue<String>(0).capacity());
        assertEquals(1, new BoundedEventQueue<String>(-5).capacity());
    }

    @Test
    void clearDiscardsWithoutTouchingTheDropCounter() {
        BoundedEventQueue<String> q = new BoundedEventQueue<>(2);
        q.offer("a");
        q.offer("b");
        q.offer("c");   // drops "a"
        q.clear();
        assertTrue(q.isEmpty());
        assertEquals(1, q.droppedCount());
    }

    @Test
    void concurrentOffersDoNotLoseOrCorruptEvents() throws Exception {
        BoundedEventQueue<Integer> q = new BoundedEventQueue<>(10_000);
        Thread[] threads = new Thread[4];
        for (int t = 0; t < threads.length; t++) {
            final int base = t * 1000;
            threads[t] = new Thread(() -> {
                for (int i = 0; i < 1000; i++) {
                    q.offer(base + i);
                }
            });
            threads[t].start();
        }
        for (Thread t : threads) {
            t.join();
        }
        assertEquals(4000, q.size());
        assertEquals(0, q.droppedCount());
        assertEquals(4000, q.drain().size());
    }
}
