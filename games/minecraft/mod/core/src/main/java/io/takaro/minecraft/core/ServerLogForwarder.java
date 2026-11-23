package io.takaro.minecraft.core;

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.core.Logger;
import org.apache.logging.log4j.core.LogEvent;
import org.apache.logging.log4j.core.appender.AbstractAppender;
import org.apache.logging.log4j.core.config.Property;

import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.function.Consumer;

/**
 * Hands the server's log lines to Takaro. All three platforms log through log4j, so one appender on
 * the root logger covers them. The connector's own lines and its socket library's stay out, or each
 * sent line would log another.
 *
 * <p>log4j calls an appender on the thread that logged, which may be the server thread. Sending from there
 * would put socket work on the game thread, and anything the send logs on that thread is refused by log4j as
 * a recursive call. So {@link #append} only queues the line, and one daemon thread sends it.
 */
final class ServerLogForwarder extends AbstractAppender {

    // Takaro drops log events beyond a rate limit and raises event-rate-limited, so stay under it.
    static final int MAX_LINES_PER_WINDOW = 40;
    static final long WINDOW_MILLIS = 30_000;
    // A burst beyond this while Takaro is slow is dropped rather than held in memory.
    static final int QUEUE_CAPACITY = 256;

    private final Consumer<String> sink;
    private final BlockingQueue<String> queue = new ArrayBlockingQueue<>(QUEUE_CAPACITY);
    private final Thread sender;
    private long windowStart;
    private int linesInWindow;

    private ServerLogForwarder(Consumer<String> sink) {
        super("TakaroServerLog", null, null, true, Property.EMPTY_ARRAY);
        this.sink = sink;
        this.sender = new Thread(this::drain, "takaro-log-forward");
        this.sender.setDaemon(true);
    }

    /** Starts forwarding until {@link #detach()}. */
    static ServerLogForwarder attach(Consumer<String> sink) {
        ServerLogForwarder forwarder = new ServerLogForwarder(sink);
        forwarder.start();
        forwarder.sender.start();
        ((Logger) LogManager.getRootLogger()).addAppender(forwarder);
        return forwarder;
    }

    void detach() {
        ((Logger) LogManager.getRootLogger()).removeAppender(this);
        stop();
        sender.interrupt();
    }

    @Override
    public void append(LogEvent event) {
        if (isOwnLine(event.getLoggerName())) return;
        if (!allow(event.getTimeMillis())) return;
        queue.offer(event.getMessage().getFormattedMessage());
    }

    private void drain() {
        while (!Thread.currentThread().isInterrupted()) {
            String line;
            try {
                line = queue.take();
            } catch (InterruptedException e) {
                return;
            }
            try {
                sink.accept(line);
            } catch (RuntimeException e) {
                // One failed send must not end forwarding; the line is lost like any line sent while offline.
            }
        }
    }

    private static boolean isOwnLine(String logger) {
        if (logger == null) return false;
        return logger.toLowerCase(Locale.ROOT).contains("takaro") || logger.startsWith("org.java_websocket");
    }

    synchronized boolean allow(long nowMillis) {
        if (nowMillis - windowStart >= WINDOW_MILLIS) {
            windowStart = nowMillis;
            linesInWindow = 0;
        }
        return ++linesInWindow <= MAX_LINES_PER_WINDOW;
    }
}
