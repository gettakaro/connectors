// Research probe for issue #298 (C4 / ZOM-2). Uses the production MainThreadQueue and mirrors
// TakaroWebSocketClient.handleOnMainThread (core/.../TakaroWebSocketClient.java:669-690): a
// request future with orTimeout, a task queued for the next tick. The timeout is scaled from 10 s
// to 200 ms. Built only by run.sh in this folder; not part of the Gradle build or CI.
import io.takaro.zomboid.agent.MainThreadQueue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class TimeoutProbe {
    public static void main(String[] args) throws Exception {
        MainThreadQueue queue = new MainThreadQueue();
        AtomicInteger itemsGranted = new AtomicInteger();  // independent mutation counter
        AtomicInteger reportedFailed = new AtomicInteger();
        int requests = 50;
        for (int i = 0; i < requests; i++) {
            CompletableFuture<String> future = new CompletableFuture<>();
            queue.runOnMainThread(() -> { itemsGranted.incrementAndGet(); future.complete("ok"); });
            future.orTimeout(200, TimeUnit.MILLISECONDS).whenComplete((r, e) -> {
                if (e != null) reportedFailed.incrementAndGet();
            });
        }
        Thread.sleep(400);  // the tick is stalled past every request's timeout
        System.out.printf("before drain: reported failed to Takaro=%d, items granted=%d%n",
                reportedFailed.get(), itemsGranted.get());
        long t0 = System.nanoTime();
        queue.drain();      // the game loop resumes: one tick drains everything, no budget
        long us = (System.nanoTime() - t0) / 1000;
        System.out.printf("after one drain (%d us): reported failed=%d, items granted=%d%n",
                us, reportedFailed.get(), itemsGranted.get());
        // Admission: no capacity check exists; queue 200000 no-op tasks while stalled.
        for (int i = 0; i < 200_000; i++) queue.runOnMainThread(() -> {});
        t0 = System.nanoTime();
        queue.drain();
        System.out.printf("200000 queued tasks admitted, drained in ONE tick in %d ms%n",
                (System.nanoTime() - t0) / 1_000_000);
        boolean bug = itemsGranted.get() == requests && reportedFailed.get() == requests;
        System.out.println(bug ? "RESULT: every timed-out request still mutated the game" : "RESULT: no late mutation");
        System.exit(bug ? 1 : 0);
    }
}
