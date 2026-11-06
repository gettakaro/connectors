package io.takaro.minecraft.core;

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.core.Logger;
import org.apache.logging.log4j.core.LogEvent;
import org.apache.logging.log4j.core.appender.AbstractAppender;
import org.apache.logging.log4j.core.config.Property;

import java.util.Locale;
import java.util.function.Consumer;

/**
 * Hands the server's log lines to Takaro. All three platforms log through log4j, so one appender on
 * the root logger covers them. The connector's own lines and its socket library's stay out, or each
 * sent line would log another.
 */
final class ServerLogForwarder extends AbstractAppender {

    // Takaro drops log events beyond a rate limit and raises event-rate-limited, so stay under it.
    static final int MAX_LINES_PER_WINDOW = 40;
    static final long WINDOW_MILLIS = 30_000;

    private final Consumer<String> sink;
    private long windowStart;
    private int linesInWindow;

    private ServerLogForwarder(Consumer<String> sink) {
        super("TakaroServerLog", null, null, true, Property.EMPTY_ARRAY);
        this.sink = sink;
    }

    /** Starts forwarding until {@link #detach()}. */
    static ServerLogForwarder attach(Consumer<String> sink) {
        ServerLogForwarder forwarder = new ServerLogForwarder(sink);
        forwarder.start();
        ((Logger) LogManager.getRootLogger()).addAppender(forwarder);
        return forwarder;
    }

    void detach() {
        ((Logger) LogManager.getRootLogger()).removeAppender(this);
        stop();
    }

    @Override
    public void append(LogEvent event) {
        if (isOwnLine(event.getLoggerName())) return;
        if (!allow(event.getTimeMillis())) return;
        sink.accept(event.getMessage().getFormattedMessage());
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
