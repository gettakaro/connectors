package dev.takaro.hytale.util;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Summarises the player identity fields of an outgoing frame for the wire trace.
 *
 * <p>Takaro matches players only on the explicit {@code steamId}, {@code epicOnlineServicesId},
 * {@code xboxLiveId} and {@code platformId} fields it receives, so a wrong or missing field silently
 * creates duplicate profiles. Logging exactly what went on the wire lets a live test judge identity
 * from the connector side instead of from Takaro's merged player record. Only identity keys are
 * printed, never chat text or other payload content.
 *
 * <p>Deliberately free of any Hytale server import so it can be unit-tested without the server jar.
 */
public final class IdentityTrace {
    private static final String[] KEYS = {"gameId", "platformId", "steamId", "epicOnlineServicesId", "xboxLiveId"};
    private static final int MAX_DEPTH = 5;
    private static final int MAX_ENTRIES = 10;

    private IdentityTrace() {
    }

    /** @return e.g. {@code [gameId=u platformId=hytale:u]}, or null when the frame carries no player. */
    public static String of(Object frame) {
        List<String> entries = new ArrayList<>();
        collect(frame, entries, 0);
        if (entries.isEmpty()) {
            return null;
        }
        return String.join(" ", entries);
    }

    private static void collect(Object node, List<String> out, int depth) {
        if (node == null || depth > MAX_DEPTH || out.size() >= MAX_ENTRIES) {
            return;
        }
        if (node instanceof Map) {
            Map<?, ?> map = (Map<?, ?>) node;
            if (map.containsKey("gameId") && (map.containsKey("platformId") || map.containsKey("steamId")
                || map.containsKey("epicOnlineServicesId") || map.containsKey("xboxLiveId"))) {
                StringBuilder sb = new StringBuilder("[");
                for (String key : KEYS) {
                    if (map.containsKey(key)) {
                        if (sb.length() > 1) {
                            sb.append(' ');
                        }
                        sb.append(key).append('=').append(map.get(key));
                    }
                }
                out.add(sb.append(']').toString());
            }
            for (Object value : map.values()) {
                collect(value, out, depth + 1);
            }
        } else if (node instanceof Iterable) {
            for (Object value : (Iterable<?>) node) {
                collect(value, out, depth + 1);
            }
        }
    }
}
