package dev.takaro.hytale.protocol;

import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Turns what an action handler returned into a protocol v1 answer.
 *
 * <p>The handlers report failure the protocol 0 way, as a map with {@code success:false} and/or
 * an {@code error} text, which v0 Takaro reads. Protocol 1 wants an error envelope with a code
 * instead, {@code payload: null} for actions that return nothing, and {@code null} for a player
 * that cannot be found. Keeping the conversion here leaves the v0 answers exactly as they were.
 */
public final class V1Results {

    /** Actions whose v1 result is empty: success is {@code payload: null}. */
    static final Set<String> VOID_ACTIONS = Set.of(
        "sendMessage", "giveItem", "kickPlayer", "banPlayer", "unbanPlayer",
        "teleportPlayer", "teleportPlayerToPlayer", "shutdown", "setPlayerNameColor");

    /** Actions whose answer is a CommandOutput: {@code success:false} there is a valid result. */
    static final Set<String> COMMAND_ACTIONS = Set.of("executeConsoleCommand", "executeCommand");

    /** Actions where an unknown player is answered with {@code payload: null}, not an error. */
    static final Set<String> NULL_WHEN_MISSING = Set.of("getPlayer", "getPlayerLocation");

    private V1Results() {
    }

    /**
     * The v1 payload for a handler result, or a {@link ProtocolException} carrying the code and
     * message of a failure.
     */
    public static Object adapt(String action, Object result) {
        if (result instanceof Map<?, ?> map && !COMMAND_ACTIONS.contains(action) && isFailure(map)) {
            String message = errorText(action, map);
            String code = classify(message);
            if (ProtocolException.NOT_FOUND.equals(code) && NULL_WHEN_MISSING.contains(action)) {
                return null;
            }
            throw new ProtocolException(code, message);
        }
        if (VOID_ACTIONS.contains(action)) {
            return null;
        }
        return result;
    }

    static boolean isFailure(Map<?, ?> map) {
        if (Boolean.FALSE.equals(map.get("success"))) {
            return true;
        }
        return map.get("error") != null && !Boolean.TRUE.equals(map.get("success"));
    }

    static String errorText(String action, Map<?, ?> map) {
        Object error = map.get("error");
        String text = error == null ? null : String.valueOf(error).trim();
        return text == null || text.isEmpty() ? action + " failed" : text;
    }

    /** The v1 error code for one of the handlers' error texts. */
    public static String classify(String message) {
        String m = message == null ? "" : message.toLowerCase(Locale.ROOT);
        if (m.startsWith("map_disabled") || m.startsWith("unknown action")) {
            return ProtocolException.UNSUPPORTED;
        }
        if (m.startsWith("invalid_args") || m.startsWith("no ") || m.contains(" provided")
            || m.contains("not a hytale uuid") || m.contains("could not parse") || m.contains(" needs ")
            || m.contains("invalid uuid")) {
            return ProtocolException.INVALID_ARGS;
        }
        if (m.startsWith("map_not_ready") || m.contains("not found") || m.contains("not online")
            || m.contains("not in world") || m.contains("not in a world") || m.contains("never been seen")) {
            return ProtocolException.NOT_FOUND;
        }
        return ProtocolException.GAME_ERROR;
    }
}
