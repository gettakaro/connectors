package dev.takaro.hytale.util;

import java.util.logging.Level;

/**
 * Decides which server log records may be forwarded to Takaro as {@code log} game events.
 *
 * <p>Two separate problems are solved here.
 *
 * <p><b>The amplification loop.</b> {@code TakaroLogHandler} subscribes to every server
 * log record and sends each one to Takaro. Anything the connector itself logs while sending a
 * frame therefore becomes another log record, which becomes another game event, forever. The
 * upstream author's workaround was to log all wire activity at {@code FINE}, which made the
 * wire unobservable in practice. Instead, wire logging goes to a dedicated logger whose
 * name is {@link #WIRE_LOGGER_NAME}, and this filter refuses to forward anything from it. That
 * breaks the loop by exclusion rather than by silence, so the wire can be logged at INFO.
 *
 * <p><b>Volume.</b> A level floor and a per-minute cap keep a chatty server from flooding the
 * Takaro socket. Suppression is counted so it can be reported honestly.
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar.
 */
public class LogForwardFilter {
    /** Logger name used for wire tracing; never forwarded to Takaro. */
    public static final String WIRE_LOGGER_NAME = "takaro.wire";

    private final int minLevel;
    private final int maxPerMinute;

    private long windowStart;
    private int inWindow;
    private long suppressed;

    public LogForwardFilter(Level minLevel, int maxPerMinute) {
        this.minLevel = (minLevel == null ? Level.INFO : minLevel).intValue();
        this.maxPerMinute = maxPerMinute <= 0 ? Integer.MAX_VALUE : maxPerMinute;
    }

    /** Parse a configured level name; unknown or blank values fall back to {@code def}. */
    public static Level parseLevel(String name, Level def) {
        if (name == null || name.trim().isEmpty()) {
            return def;
        }
        try {
            return Level.parse(name.trim().toUpperCase(java.util.Locale.ROOT));
        } catch (IllegalArgumentException e) {
            return def;
        }
    }

    /** True when a record from this logger must never be forwarded. */
    public static boolean isExcludedLogger(String loggerName) {
        return loggerName != null && loggerName.startsWith(WIRE_LOGGER_NAME);
    }

    /**
     * Should this record be forwarded?
     *
     * @param loggerName record's logger name (may be null)
     * @param level      record's level (may be null)
     * @param now        current time in millis, injected so the rate cap is testable
     */
    public synchronized boolean shouldForward(String loggerName, Level level, long now) {
        if (isExcludedLogger(loggerName)) {
            return false;
        }
        int value = level == null ? Level.INFO.intValue() : level.intValue();
        if (value < minLevel) {
            return false;
        }

        if (now - windowStart >= 60_000L) {
            windowStart = now;
            inWindow = 0;
        }
        if (inWindow >= maxPerMinute) {
            suppressed++;
            return false;
        }
        inWindow++;
        return true;
    }

    /** Records not forwarded because the per-minute cap was hit, since startup. */
    public synchronized long suppressedCount() {
        return suppressed;
    }

    public int maxPerMinute() {
        return maxPerMinute;
    }
}
