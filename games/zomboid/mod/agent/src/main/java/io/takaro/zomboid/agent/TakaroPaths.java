package io.takaro.zomboid.agent;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Where the connector keeps its files: the folder that holds the agent jar
 * ({@code <Zomboid>/Takaro/}), so the config sits next to the jar on Linux and Windows
 * alike. {@code -Dtakaro.configFile}, {@code -Dtakaro.logFile} and
 * {@code -Dtakaro.bansFile} override each file.
 *
 * <p>Must not log: {@link AgentLog} asks this class for its own file.
 */
public final class TakaroPaths {

    public static final String CONFIG_NAME = "TakaroConfig.txt";

    private static volatile Path dataDir;

    private TakaroPaths() {
    }

    public static Path dataDir() {
        Path dir = dataDir;
        if (dir == null) {
            dir = resolveDataDir(System.getProperty("takaro.configFile"), agentJarDir(),
                    System.getProperty("user.home"));
            dataDir = dir;
        }
        return dir;
    }

    public static Path configFile() {
        String override = System.getProperty("takaro.configFile");
        return override != null && !override.isEmpty() ? Paths.get(override) : dataDir().resolve(CONFIG_NAME);
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
     * The config override's folder, else the jar's folder, else {@code <home>/Zomboid/Takaro}
     * (only when the classes do not come from a jar, as in the tests).
     */
    static Path resolveDataDir(String configOverride, Path jarDir, String userHome) {
        if (configOverride != null && !configOverride.isEmpty()) {
            Path parent = Paths.get(configOverride).toAbsolutePath().getParent();
            if (parent != null) {
                return parent;
            }
        }
        if (jarDir != null) {
            return jarDir;
        }
        return Paths.get(userHome != null ? userHome : "", "Zomboid", "Takaro");
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
