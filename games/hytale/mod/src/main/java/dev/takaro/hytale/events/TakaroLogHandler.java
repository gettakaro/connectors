package dev.takaro.hytale.events;

import dev.takaro.hytale.TakaroPlugin;

import java.util.HashMap;
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

    public TakaroLogHandler(TakaroPlugin plugin) {
        this.plugin = plugin;
        this.logBuffer = new CopyOnWriteArrayList<>();
        this.scheduler = Executors.newSingleThreadScheduledExecutor();
    }

    /**
     * Start capturing and forwarding logs
     */
    public void start() {
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
        if (logBuffer.isEmpty()) {
            return;
        }

        try {
            // Take up to BATCH_SIZE logs
            int count = Math.min(BATCH_SIZE, logBuffer.size());
            for (int i = 0; i < count; i++) {
                LogRecord record = logBuffer.remove(0);
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
