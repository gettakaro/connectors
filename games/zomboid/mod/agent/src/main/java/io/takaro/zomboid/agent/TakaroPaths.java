package io.takaro.zomboid.agent;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Predicate;

/**
 * Where the connector keeps its files: the folder that holds the agent jar
 * ({@code <Zomboid>/Takaro/}), so the config sits next to the jar on Linux and Windows
 * alike.
 *
 * <p>An install whose files already live in {@code ~/Zomboid/Takaro} or
 * {@code /home/steam/Zomboid/Takaro} (every release up to now used that fixed path) keeps
 * using that folder, even when the jar is elsewhere. {@code -Dtakaro.configFile},
 * {@code -Dtakaro.logFile} and {@code -Dtakaro.bansFile} still override each file.
 *
 * <p>Must not log: {@link AgentLog} asks this class for its own file.
 */
public final class TakaroPaths {

    public static final String CONFIG_NAME = "TakaroConfig.txt";
    public static final String SAVED_NAME = "TakaroConfig.saved.txt";
    public static final String LEGACY_DIR = "/home/steam/Zomboid/Takaro";

    /** Files whose presence marks a folder an earlier connector already used. */
    private static final String[] MARKERS = {CONFIG_NAME, SAVED_NAME, "bans.json"};

    private static volatile Path dataDir;

    private TakaroPaths() {
    }

    public static Path dataDir() {
        Path dir = dataDir;
        if (dir == null) {
            dir = resolveDataDir(System.getProperty("takaro.configFile"), agentJarDir(),
                    System.getProperty("user.home"), Files::exists);
            dataDir = dir;
        }
        return dir;
    }

    public static Path configFile() {
        String override = System.getProperty("takaro.configFile");
        return override != null && !override.isEmpty() ? Paths.get(override) : dataDir().resolve(CONFIG_NAME);
    }

    /** The connector-written copy of the tokens in use, beside the config file. */
    public static Path savedFile() {
        Path config = configFile();
        Path parent = config.toAbsolutePath().getParent();
        return parent != null ? parent.resolve(SAVED_NAME) : Paths.get(SAVED_NAME);
    }

    public static Path logFile() {
        String override = System.getProperty("takaro.logFile");
        return override != null && !override.isEmpty() ? Paths.get(override) : dataDir().resolve("takaro-agent.log");
    }

    public static Path bansFile() {
        String override = System.getProperty("takaro.bansFile");
        return override != null && !override.isEmpty() ? Paths.get(override) : dataDir().resolve("bans.json");
    }

    /**
     * The folder to use: the config override's folder; else the first of the jar's folder,
     * {@code <home>/Zomboid/Takaro} and {@code /home/steam/Zomboid/Takaro} that already
     * holds connector files; else the jar's folder; else {@code <home>/Zomboid/Takaro}.
     */
    static Path resolveDataDir(String configOverride, Path jarDir, String userHome, Predicate<Path> exists) {
        if (configOverride != null && !configOverride.isEmpty()) {
            Path parent = Paths.get(configOverride).toAbsolutePath().getParent();
            if (parent != null) {
                return parent;
            }
        }
        Path homeDir = userHome != null && !userHome.isEmpty()
                ? Paths.get(userHome, "Zomboid", "Takaro") : null;
        List<Path> candidates = new ArrayList<>();
        if (jarDir != null) {
            candidates.add(jarDir);
        }
        if (homeDir != null) {
            candidates.add(homeDir);
        }
        candidates.add(Paths.get(LEGACY_DIR));
        for (Path candidate : candidates) {
            for (String marker : MARKERS) {
                if (exists.test(candidate.resolve(marker))) {
                    return candidate;
                }
            }
        }
        if (jarDir != null) {
            return jarDir;
        }
        return homeDir != null ? homeDir : Paths.get(LEGACY_DIR);
    }

    /** The folder of the jar this class was loaded from, or null outside a jar (tests). */
    static Path agentJarDir() {
        try {
            var source = TakaroPaths.class.getProtectionDomain().getCodeSource();
            if (source == null || source.getLocation() == null) {
                return null;
            }
            Path location = Paths.get(source.getLocation().toURI()).toAbsolutePath();
            if (Files.isRegularFile(location) && location.getFileName().toString().endsWith(".jar")) {
                return location.getParent();
            }
        } catch (Exception | Error ignored) {
            // unreadable code source: fall back to the home folder
        }
        return null;
    }
}
