package dev.takaro.hytale.util;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Pure assembly rules for the text {@code executeConsoleCommand} returns.
 *
 * <p>A Takaro-issued console command produces output in two places:
 * <ol>
 *   <li>everything the command writes back to its {@code CommandSender} (the authoritative half -
 *       Hytale's own console sender is just a renderer around the same messages), and</li>
 *   <li>log records a command emits directly through the server logger while it runs.</li>
 * </ol>
 * Half (2) is only admissible when it can be attributed to the thread(s) that executed
 * <em>this</em> command - taking the global log stream is how other people's output ended up
 * in a Takaro response. This class does the part that needs no Hytale classes:
 * merging, de-duplication, exclusion and capping.
 *
 * <p>No Hytale imports, so it is unit-testable without the server jar.
 */
public final class CommandOutput {
    /** Hard cap on the number of lines returned. */
    public static final int MAX_LINES = 400;
    /** Hard cap on the number of characters returned. */
    public static final int MAX_CHARS = 16_000;

    private static final String TRUNCATED = "... [output truncated by the Takaro connector]";

    private CommandOutput() {
    }

    /**
     * True for a log line that must never be part of a command's answer, whatever thread it
     * came from:
     * <ul>
     *   <li>{@code CommandManager}'s own {@code "<sender> executed command: <cmd>"} /
     *       {@code "<sender> sent command: <cmd>"} echo - it is about the command, not its
     *       output, and it is exactly the noise that used to leak in;</li>
     *   <li>anything the connector itself logged while handling the request.</li>
     * </ul>
     */
    public static boolean isExcluded(String line) {
        if (line == null || line.isBlank()) {
            return true;
        }
        String lower = line.toLowerCase(Locale.ROOT);
        return lower.contains("executed command:")
            || lower.contains("sent command:")
            || lower.contains("[wire]");
    }

    /**
     * True for a logger whose records must never be merged into command output: the connector's
     * own wire logger, and the connector's own package.
     */
    public static boolean isExcludedLogger(String loggerName) {
        if (loggerName == null) {
            return false;
        }
        return loggerName.startsWith(LogForwardFilter.WIRE_LOGGER_NAME)
            || loggerName.startsWith("dev.takaro.hytale");
    }

    /**
     * Merge the sender half and the thread-attributed logger half in order, dropping excluded
     * and duplicate lines, then cap the result.
     *
     * @param senderLines what the command wrote back to its own {@code CommandSender}
     * @param loggerLines log records attributed to the thread(s) that executed the command
     */
    public static String merge(List<String> senderLines, List<String> loggerLines) {
        Set<String> seen = new LinkedHashSet<>();
        List<String> out = new ArrayList<>();

        for (List<String> source : List.of(
                senderLines == null ? List.<String>of() : senderLines,
                loggerLines == null ? List.<String>of() : loggerLines)) {
            for (String raw : source) {
                for (String line : splitLines(raw)) {
                    String trimmed = stripTrailing(line);
                    if (isExcluded(trimmed)) {
                        continue;
                    }
                    if (!seen.add(trimmed)) {
                        continue;
                    }
                    out.add(trimmed);
                }
            }
        }
        return cap(out);
    }

    private static List<String> splitLines(String raw) {
        if (raw == null) {
            return List.of();
        }
        String stripped = Responses.stripAnsi(raw);
        if (stripped.indexOf('\n') < 0 && stripped.indexOf('\r') < 0) {
            return List.of(stripped);
        }
        return List.of(stripped.split("\\r?\\n", -1));
    }

    private static String stripTrailing(String line) {
        return line == null ? "" : line.replaceAll("\\s+$", "");
    }

    /** Apply the line and character caps, appending a visible marker when anything was cut. */
    static String cap(List<String> lines) {
        boolean truncated = false;
        List<String> kept = lines;
        if (kept.size() > MAX_LINES) {
            kept = kept.subList(0, MAX_LINES);
            truncated = true;
        }
        StringBuilder sb = new StringBuilder();
        for (String line : kept) {
            if (sb.length() + line.length() + 1 > MAX_CHARS) {
                truncated = true;
                break;
            }
            if (sb.length() > 0) {
                sb.append('\n');
            }
            sb.append(line);
        }
        if (truncated) {
            if (sb.length() > 0) {
                sb.append('\n');
            }
            sb.append(TRUNCATED);
        }
        return sb.toString();
    }
}
