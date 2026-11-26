package dev.takaro.hytale.events;

import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.util.LogForwardFilter;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.logging.LogRecord;

/**
 * Captures Hytale server console logs and forwards them to Takaro
 * Uses Hytale's subscriber pattern to intercept all log messages
 */
public class TakaroLogHandler {
    private final TakaroPlugin plugin;
    private final CopyOnWriteArrayList<LogRecord> logBuffer;
    private final ScheduledExecutorService scheduler;
    private static final int BATCH_SIZE = 50; // Send max 50 logs per batch
    private static final long SEND_INTERVAL_MS = 2000; // Send every 2 seconds
    private static final int MAX_BUFFERED_RECORDS = 2000; // Hard cap on the ingest backlog
    private static final long DROP_LOG_INTERVAL_MS = 30_000L;

    private LogForwardFilter filter;
    private boolean forwardingDisabled = false;
    private long droppedRecords = 0;
    private long lastDropLogAt = 0L;

    public TakaroLogHandler(TakaroPlugin plugin) {
        this.plugin = plugin;
        this.logBuffer = new CopyOnWriteArrayList<>();
        this.scheduler = Executors.newSingleThreadScheduledExecutor();
    }

    /**
     * Start capturing and forwarding logs
     */
    public void start() {
        java.util.logging.Level min = LogForwardFilter.parseLevel(
            plugin.getConfig().getLogForwardLevel(), java.util.logging.Level.INFO);
        this.forwardingDisabled = java.util.logging.Level.OFF.equals(min);
        this.filter = new LogForwardFilter(min, plugin.getConfig().getLogForwardMaxPerMin());
        if (forwardingDisabled) {
            plugin.getLogger().at(java.util.logging.Level.INFO).log(
                "Takaro log forwarding is disabled (LOG_FORWARD_LEVEL=OFF)");
            return;
        }
        plugin.getLogger().at(java.util.logging.Level.INFO).log(
            "Takaro log forwarding: level >= " + min.getName()
                + ", max " + plugin.getConfig().getLogForwardMaxPerMin() + "/min");
        // Start periodic log forwarding
        scheduler.scheduleAtFixedRate(this::forwardLogs, SEND_INTERVAL_MS, SEND_INTERVAL_MS, TimeUnit.MILLISECONDS);
        plugin.getLogger().at(java.util.logging.Level.INFO).log("Started Takaro log forwarding");
    }

    /**
     * Stop capturing logs
     */
    public void stop() {
        scheduler.shutdownNow();
        forwardLogs(); // Send any remaining logs
        plugin.getLogger().at(java.util.logging.Level.INFO).log("Stopped Takaro log forwarding");
    }

    /**
     * Get the log buffer that Hytale's logger will write to
     */
    public CopyOnWriteArrayList<LogRecord> getLogBuffer() {
        return logBuffer;
    }

    /**
     * Forward accumulated logs to Takaro
     */
    private void forwardLogs() {
        if (forwardingDisabled) {
            logBuffer.clear();
            return;
        }
        if (logBuffer.isEmpty()) {
            return;
        }

        try {
            // The ingest list must stay a CopyOnWriteArrayList because that is what
            // HytaleLoggerBackend.subscribe() takes. Draining it with remove(0) copied the
            // whole backing array once per record - quadratic, and on a server logging faster
            // than the drain rate the list grew without bound. Take a snapshot and clear the
            // drained prefix in ONE structural modification instead.
            List<LogRecord> batch = new ArrayList<>(logBuffer);
            int count = Math.min(BATCH_SIZE, batch.size());
            logBuffer.subList(0, count).clear();

            // Whatever is still queued beyond the cap is a real backlog: bound it and say so.
            int overflow = logBuffer.size() - MAX_BUFFERED_RECORDS;
            if (overflow > 0) {
                logBuffer.subList(0, overflow).clear();
                droppedRecords += overflow;
                long now = System.currentTimeMillis();
                if (now - lastDropLogAt > DROP_LOG_INTERVAL_MS) {
                    lastDropLogAt = now;
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log(
                        "Log forwarding cannot keep up - dropped " + droppedRecords
                            + " record(s) in total (buffer cap " + MAX_BUFFERED_RECORDS + ")");
                }
            }

            long now = System.currentTimeMillis();
            for (int i = 0; i < count; i++) {
                LogRecord record = batch.get(i);
                // Records from the wire logger are never forwarded - that is what stops the
                // log -> gameEvent -> log amplification loop.
                if (filter != null && !filter.shouldForward(record.getLoggerName(), record.getLevel(), now)) {
                    continue;
                }
                sendLogToTakaro(record);
            }
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log("Error forwarding logs: " + e.getMessage());
        }
    }

    /**
     * Send a single log record to Takaro
     */
    private void sendLogToTakaro(LogRecord record) {
        try {
            // Format the log message
            String loggerName = record.getLoggerName() != null ? record.getLoggerName() : "Hytale";
            String level = record.getLevel().getName();
            String message = record.getMessage();

            // Build formatted log line
            String formattedLog = String.format("[%s] [%s] %s", level, loggerName, message);

            // Build log event for Takaro
            Map<String, Object> logData = new HashMap<>();
            logData.put("msg", formattedLog);

            // Send to all Takaro connections (production and dev if enabled)
            plugin.sendGameEventToAll("log", logData);

        } catch (Exception e) {
            // Don't log errors here to avoid infinite loop
        }
    }
}
