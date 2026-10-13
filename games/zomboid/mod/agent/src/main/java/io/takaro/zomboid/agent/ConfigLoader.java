package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;

import java.io.IOException;
import java.nio.file.Path;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.function.Supplier;

/**
 * Loads {@code TakaroConfig.txt} from the connector folder ({@link TakaroPaths}) into a
 * {@link TakaroConfig}, with the {@code TAKARO_*} environment winning over the file, and
 * keeps watching the file so a pasted or corrected token applies without a restart.
 *
 * <p>Recognised keys: {@code wsUrl}, {@code registrationToken}, {@code identityToken},
 * {@code serverChatName}, {@code debug}, {@code logEvents}, {@code debugCatalog}.
 * Environment overrides: {@code TAKARO_WS_URL}, {@code TAKARO_REGISTRATION_TOKEN},
 * {@code TAKARO_IDENTITY_TOKEN}, {@code TAKARO_SERVER_CHAT_NAME}, {@code TAKARO_DEBUG},
 * {@code TAKARO_LOG_EVENTS}, {@code TAKARO_DEBUG_CATALOG}.
 *
 * <p>The only thing the connector ever writes into the file is a missing file (the
 * bundled template) or a missing {@code identityToken} (a new UUID). Three phases,
 * because {@code premain} runs in every JVM of the launch chain: {@link #load()} only
 * reads (any JVM); {@link #prepareForStart()} does those writes (the real server JVM,
 * first tick); {@link #poll()} re-reads on the watcher thread.
 */
public final class ConfigLoader {

    static final long POLL_MILLIS = 5000L;
    /** A change is applied only when a second read this much later sees the same text. */
    static final long SETTLE_MILLIS = 300L;

    private final Map<String, String> env;
    private final Supplier<String> newIdentity;
    private final Path configFile;

    private ConfigFiles.Resolved current;
    private String lastText;
    private volatile Thread watcher;

    public ConfigLoader() {
        this(System.getenv(), () -> UUID.randomUUID().toString(), TakaroPaths.configFile());
    }

    ConfigLoader(Map<String, String> env, Supplier<String> newIdentity, Path configFile) {
        this.env = env;
        this.newIdentity = newIdentity;
        this.configFile = configFile;
    }

    public Path configFile() {
        return configFile;
    }

    public boolean isLogEvents() {
        return current != null && current.logEvents;
    }

    /**
     * When set, the connector logs the {@code listItems}/{@code listEntities}/
     * {@code listLocations} sizes once at start. Config key {@code debugCatalog}, env
     * {@code TAKARO_DEBUG_CATALOG}, or system property {@code takaro.debugCatalog}.
     */
    public boolean isDebugCatalog() {
        String prop = System.getProperty("takaro.debugCatalog");
        if (prop != null && !prop.isEmpty()) {
            return ConfigFiles.isTruthy(prop);
        }
        return current != null && current.debugCatalog;
    }

    /** Reads the file without writing anything. */
    public synchronized TakaroConfig load() {
        String text = null;
        try {
            text = ConfigFiles.readOrNull(configFile);
            AgentLog.log(text != null
                    ? "config: loaded " + configFile
                    : "config: " + configFile + " not present yet");
        } catch (IOException | RuntimeException e) {
            AgentLog.error("config: failed to read " + configFile, e);
        }
        current = resolve(text);
        lastText = text;
        return current.config;
    }

    /**
     * The real server JVM, before the first connect: create the file when it is missing
     * and give it an identity when it has none.
     */
    public synchronized ConfigFiles.Resolved prepareForStart() {
        String text;
        try {
            text = ConfigFiles.readOrNull(configFile);
        } catch (IOException | RuntimeException e) {
            // Unreadable is not absent: writing now could replace a real file.
            AgentLog.error("config: failed to read " + configFile + "; using what premain loaded", e);
            return current != null ? current : resolve(null);
        }
        String created = null;
        if (text == null) {
            try {
                created = ConfigFiles.template();
            } catch (IOException e) {
                AgentLog.error("config: could not create " + configFile, e);
            }
        }
        String written = withIdentityIfMissing(created != null ? created : text, text, newIdentity);
        if (written != null) {
            AgentLog.log(created != null
                    ? "config: created " + configFile + " with a new identityToken (paste the registration token into it)"
                    : "config: no identityToken yet; wrote a new one into " + configFile);
            text = written;
        }
        current = resolve(text);
        lastText = text;
        return current;
    }

    /**
     * One watcher tick. Returns the new settings when the config file's text changed and
     * a second read shortly after saw the same text; null otherwise (unchanged, mid-save,
     * missing or unreadable: the current settings stay and the next tick looks again).
     */
    public synchronized ConfigFiles.Resolved poll() {
        String text;
        try {
            text = ConfigFiles.readOrNull(configFile);
            if (text == null || Objects.equals(text, lastText)) {
                return null;
            }
            Thread.sleep(SETTLE_MILLIS);
            if (!Objects.equals(text, ConfigFiles.readOrNull(configFile))) {
                return null;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return null;
        } catch (IOException | RuntimeException e) {
            return null;
        }

        AgentLog.log("config: " + configFile + " changed");
        // Emptying identityToken would otherwise make the next start create a second record.
        String inUse = ConfigFiles.parse(lastText).get("identityToken");
        if (TakaroConfig.isSet(inUse)) {
            String written = withIdentityIfMissing(text, text, () -> inUse);
            if (written != null) {
                AgentLog.log("config: identityToken was emptied; wrote the one in use back");
                text = written;
            }
        }
        current = resolve(text);
        lastText = text;
        return current;
    }

    /** Polls every few seconds on a daemon thread and hands each change to {@code onChange}. */
    public void startWatching(Consumer<ConfigFiles.Resolved> onChange) {
        if (watcher != null) {
            return;
        }
        Thread t = new Thread(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    Thread.sleep(POLL_MILLIS);
                    ConfigFiles.Resolved changed = poll();
                    if (changed != null) {
                        onChange.accept(changed);
                    }
                } catch (InterruptedException e) {
                    return;
                } catch (Throwable t2) {
                    AgentLog.error("config: watcher tick failed", t2);
                }
            }
        }, "takaro-config-watch");
        t.setDaemon(true);
        watcher = t;
        t.start();
    }

    public void stopWatching() {
        Thread t = watcher;
        if (t != null) {
            t.interrupt();
        }
    }

    private ConfigFiles.Resolved resolve(String text) {
        ConfigFiles.Resolved resolved = ConfigFiles.resolve(ConfigFiles.parse(text), env);
        resolved.config.setConfigFileHint(configFile.toAbsolutePath().toString());
        return resolved;
    }

    /**
     * Writes {@code text} with an identity from {@code identity} when it has none and the
     * file still holds {@code onDisk} (null: still absent), so a save made meanwhile is
     * never overwritten. Returns what it wrote, or null when it wrote nothing.
     */
    private String withIdentityIfMissing(String text, String onDisk, Supplier<String> identity) {
        if (text == null || TakaroConfig.isSet(ConfigFiles.parse(text).get("identityToken"))) {
            return null;
        }
        String updated = ConfigFiles.withIdentity(text, identity.get());
        try {
            if (!Objects.equals(onDisk, ConfigFiles.readOrNull(configFile))) {
                return null; // saved again meanwhile: the next poll sees that text
            }
            ConfigFiles.writeAtomically(configFile, updated);
            return updated;
        } catch (IOException | RuntimeException e) {
            AgentLog.error("config: could not write identityToken into " + configFile, e);
            return null;
        }
    }
}
