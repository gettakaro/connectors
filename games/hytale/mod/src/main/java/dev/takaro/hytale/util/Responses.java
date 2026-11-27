package dev.takaro.hytale.util;

import java.util.HashMap;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Pure helpers for shaping the payloads Takaro expects.
 *
 * <p>Deliberately free of any Hytale server import so it can be unit-tested without the
 * server jar on the classpath.
 */
public final class Responses {
    /** Matches ANSI SGR / CSI escape sequences. */
    private static final Pattern ANSI = Pattern.compile("\\[[;\\d]*[ -/]*[@-~]");

    private Responses() {
    }

    /**
     * Build the {@code {success, rawResult}} shape Takaro's {@code CommandOutput} DTO requires.
     * {@code rawResult} must always be a string or Takaro rejects the frame with a 400.
     */
    public static Map<String, Object> commandResult(boolean success, String rawResult) {
        Map<String, Object> result = new HashMap<>();
        result.put("success", success);
        result.put("rawResult", rawResult == null ? "" : rawResult);
        return result;
    }

    /** The command-name portion of a console command line, without a leading slash. */
    public static String commandName(String command) {
        if (command == null) {
            return "";
        }
        String trimmed = command.trim();
        int space = trimmed.indexOf(' ');
        String name = space < 0 ? trimmed : trimmed.substring(0, space);
        return name.startsWith("/") ? name.substring(1) : name;
    }

    /** Remove ANSI escape sequences so captured console output is plain text. */
    public static String stripAnsi(String text) {
        return text == null ? "" : ANSI.matcher(text).replaceAll("");
    }
}
