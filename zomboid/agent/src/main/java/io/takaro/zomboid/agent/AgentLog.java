package io.takaro.zomboid.agent;

import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;

/**
 * Dead-simple appender for the agent log. Writes to
 * {@code /home/steam/Zomboid/Takaro/takaro-agent.log} and mirrors to stdout so
 * the lines also show up in {@code docker logs}.
 *
 * <p>Deliberately dependency-free: this runs inside {@code premain}, before the
 * game has initialised anything, and must never throw into game code.
 */
public final class AgentLog {

    private static final DateTimeFormatter TS =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss.SSS");

    /** Overridable for tests / non-standard installs. */
    private static final Path LOG_FILE = Paths.get(
            System.getProperty("takaro.logFile", "/home/steam/Zomboid/Takaro/takaro-agent.log"));

    private static volatile boolean logFileUsable = true;

    private AgentLog() {
    }

    public static void log(String message) {
        String line = "[" + TS.format(ZonedDateTime.now()) + "] [Takaro] " + message;
        System.out.println(line);
        if (!logFileUsable) {
            return;
        }
        try {
            Path parent = LOG_FILE.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            Files.write(LOG_FILE, (line + System.lineSeparator()).getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException | RuntimeException e) {
            logFileUsable = false;
            System.out.println("[Takaro] log file unusable (" + e + "), stdout only");
        }
    }

    public static void error(String message, Throwable t) {
        StringWriter sw = new StringWriter();
        t.printStackTrace(new PrintWriter(sw));
        log("ERROR " + message + System.lineSeparator() + sw);
    }
}
