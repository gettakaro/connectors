package dev.takaro.hytale.websocket;

import com.hypixel.hytale.logger.backend.HytaleLoggerBackend;
import dev.takaro.hytale.util.LogForwardFilter;

import java.util.logging.Level;
import java.util.logging.LogRecord;

/**
 * Traces every WebSocket frame at INFO when {@code TAKARO_DEBUG=true}.
 *
 * <p>Before this, the only wire logging was at {@code FINE} and carried no {@code requestId},
 * so a successful request left no trace at all in the server log - and raising the level
 * was impossible anyway, because the log-forwarding handler turned every connector log line
 * into another game event, which logged another line.
 *
 * <p>These records are emitted through a dedicated logger, {@link LogForwardFilter#WIRE_LOGGER_NAME},
 * which the forwarder refuses to forward. The loop is broken by exclusion, so the wire can be
 * logged loudly and still be safe against a live Takaro socket.
 */
public final class WireLogger {
    private static final HytaleLoggerBackend LOGGER =
        HytaleLoggerBackend.getLogger(LogForwardFilter.WIRE_LOGGER_NAME);

    public static final String IN = "IN ";
    public static final String OUT = "OUT";

    private WireLogger() {
    }

    /**
     * @param direction {@link #IN} or {@link #OUT}
     * @param type      frame type (identify, request, response, gameEvent, ping, pong, error)
     * @param requestId request id, or null
     * @param action    action or event name, or null
     */
    public static void frame(boolean enabled, String prefix, String direction,
                             String type, String requestId, String action) {
        frame(enabled, prefix, direction, type, requestId, action, null);
    }

    /**
     * @param identity player identity fields carried by the frame ({@link dev.takaro.hytale.util.IdentityTrace}),
     *                 or null
     */
    public static void frame(boolean enabled, String prefix, String direction,
                             String type, String requestId, String action, String identity) {
        if (!enabled) {
            return;
        }
        try {
            StringBuilder sb = new StringBuilder();
            sb.append(prefix == null ? "" : prefix).append("[wire] ").append(direction)
                .append(" type=").append(type == null ? "?" : type);
            if (requestId != null) {
                sb.append(" requestId=").append(requestId);
            }
            if (action != null) {
                sb.append(" action=").append(action);
            }
            if (identity != null) {
                sb.append(" ids=").append(identity);
            }
            LogRecord record = new LogRecord(Level.INFO, sb.toString());
            record.setLoggerName(LogForwardFilter.WIRE_LOGGER_NAME);
            LOGGER.log(record);
        } catch (Exception ignored) {
            // Tracing must never break the connection.
        }
    }
}
