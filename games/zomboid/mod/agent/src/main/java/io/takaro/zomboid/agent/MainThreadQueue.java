package io.takaro.zomboid.agent;

import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * A queue of tasks to run on the Project Zomboid server's main thread.
 *
 * <p>Anything that reads or mutates {@code zombie.network.GameServer} state must
 * happen on the main thread. WebSocket-thread code offers a {@link Runnable}
 * here and {@link #drain()} — called from the tick hook on the main thread —
 * runs them. {@link #runOnMainThread} runs inline when already on the main
 * thread so an action triggered from within a tick does not deadlock waiting for
 * the next tick.
 */
public final class MainThreadQueue {

    private final ConcurrentLinkedQueue<Runnable> queue = new ConcurrentLinkedQueue<>();
    private volatile Thread mainThread;

    /** Records the main thread once it is known (the first tick). */
    public void setMainThread(Thread t) {
        this.mainThread = t;
    }

    /** Run inline if we are already on the main thread, else enqueue for the next drain. */
    public void runOnMainThread(Runnable task) {
        Thread mt = mainThread;
        if (mt != null && Thread.currentThread() == mt) {
            safeRun(task);
        } else {
            queue.offer(task);
        }
    }

    /** Drains and runs every queued task. Called on the main thread from the tick hook. */
    public void drain() {
        Runnable r;
        while ((r = queue.poll()) != null) {
            safeRun(r);
        }
    }

    private static void safeRun(Runnable task) {
        try {
            task.run();
        } catch (Throwable t) {
            AgentLog.error("main-thread task failed", t);
        }
    }
}
