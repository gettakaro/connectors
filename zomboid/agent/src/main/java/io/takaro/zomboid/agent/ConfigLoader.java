package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;

/**
 * Loads {@code /home/steam/Zomboid/Takaro/TakaroConfig.txt} (simple
 * {@code key=value} with {@code #} comments) into a {@link TakaroConfig}, then
 * applies environment overrides so the compose env wins over the file.
 *
 * <p>Recognised keys: {@code wsUrl}, {@code identityToken},
 * {@code registrationToken}, {@code debug}, {@code logEvents}. Environment
 * overrides (applied last): {@code TAKARO_WS_URL}, {@code TAKARO_IDENTITY_TOKEN},
 * {@code TAKARO_REGISTRATION_TOKEN}, {@code TAKARO_DEBUG}, {@code TAKARO_LOG_EVENTS}.
 */
public final class ConfigLoader {

    public static final String DEFAULT_CONFIG_FILE = "/home/steam/Zomboid/Takaro/TakaroConfig.txt";

    private boolean logEvents;
    private boolean debugCatalog;

    public boolean isLogEvents() {
        return logEvents;
    }

    /**
     * When set, the connector logs the {@code listItems}/{@code listEntities}/
     * {@code listLocations} sizes once at start (a server-side way to prove the
     * catalogue when no Takaro REST driver is available). Config key
     * {@code debugCatalog}, env {@code TAKARO_DEBUG_CATALOG}, or system property
     * {@code takaro.debugCatalog}.
     */
    public boolean isDebugCatalog() {
        return debugCatalog;
    }

    /** Resolved per call (not cached) so a test/install can point it elsewhere via the system property. */
    public static Path configFile() {
        return Paths.get(System.getProperty("takaro.configFile", DEFAULT_CONFIG_FILE));
    }

    public TakaroConfig load() {
        TakaroConfig config = new TakaroConfig();
        Path configFile = configFile();
        try {
            if (Files.isReadable(configFile)) {
                List<String> lines = Files.readAllLines(configFile, StandardCharsets.UTF_8);
                for (String raw : lines) {
                    String line = stripComment(raw).trim();
                    if (line.isEmpty()) {
                        continue;
                    }
                    int eq = line.indexOf('=');
                    if (eq <= 0) {
                        continue;
                    }
                    String key = line.substring(0, eq).trim();
                    String value = line.substring(eq + 1).trim();
                    applyKey(config, key, value);
                }
                AgentLog.log("config: loaded " + configFile);
            } else {
                AgentLog.log("config: " + configFile + " not present, using env only");
            }
        } catch (Exception e) {
            AgentLog.error("config: failed to read " + configFile, e);
        }

        config.applyEnvOverrides();
        applyLogEventsEnv();
        applyDebugCatalogEnv();
        return config;
    }

    private void applyDebugCatalogEnv() {
        String env = System.getenv("TAKARO_DEBUG_CATALOG");
        if (env != null && !env.isEmpty()) {
            this.debugCatalog = isTruthy(env);
        }
        String prop = System.getProperty("takaro.debugCatalog");
        if (prop != null && !prop.isEmpty()) {
            this.debugCatalog = isTruthy(prop);
        }
    }

    private void applyKey(TakaroConfig config, String key, String value) {
        switch (key) {
            case "wsUrl":
                config.setWsUrl(value);
                break;
            case "identityToken":
                config.setIdentityToken(value);
                break;
            case "registrationToken":
                config.setRegistrationToken(value);
                break;
            case "serverChatName":
                config.setServerChatName(value);
                break;
            case "debug":
                config.setDebugEnabled(isTruthy(value));
                break;
            case "logEvents":
                this.logEvents = isTruthy(value);
                break;
            case "debugCatalog":
                this.debugCatalog = isTruthy(value);
                break;
            default:
                // ignore unknown keys so a newer config file does not break us
                break;
        }
    }

    private void applyLogEventsEnv() {
        String env = System.getenv("TAKARO_LOG_EVENTS");
        if (env != null && !env.isEmpty()) {
            this.logEvents = isTruthy(env);
        }
    }

    private static String stripComment(String line) {
        int hash = line.indexOf('#');
        return hash >= 0 ? line.substring(0, hash) : line;
    }

    private static boolean isTruthy(String v) {
        return "true".equalsIgnoreCase(v) || "1".equals(v) || "yes".equalsIgnoreCase(v);
    }
}
