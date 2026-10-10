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
 * <p>Three phases, because {@code premain} runs in every JVM of the launch chain:
 * {@link #load()} only reads (any JVM); {@link #prepareForStart()} writes the template,
 * a new identity and the saved copy (the real server JVM, first tick); {@link #poll()}
 * re-reads on the watcher thread.
 */
public final class ConfigLoader {

    static final long POLL_MILLIS = 5000L;
    /** A change is applied only when a second read this much later sees the same text. */
    static final long SETTLE_MILLIS = 300L;

    private final Map<String, String> env;
    private final Supplier<String> newIdentity;
    private final Path configFile;
    private final Path savedFile;

    private ConfigFiles.Resolved current;
    private String lastUserText;
    private volatile Thread watcher;

    public ConfigLoader() {
        this(System.getenv(), () -> UUID.randomUUID().toString(),
                TakaroPaths.configFile(), TakaroPaths.savedFile());
    }

    ConfigLoader(Map<String, String> env, Supplier<String> newIdentity, Path configFile, Path savedFile) {
        this.env = env;
        this.newIdentity = newIdentity;
        this.configFile = configFile;
        this.savedFile = savedFile;
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

    /** Reads both files without writing anything or making up an identity. */
    public synchronized TakaroConfig load() {
        String userText = null;
        String savedText = null;
        try {
            userText = ConfigFiles.readOrNull(configFile);
            AgentLog.log(userText != null
                    ? "config: loaded " + configFile
                    : "config: " + configFile + " not present yet");
        } catch (IOException | RuntimeException e) {
            AgentLog.error("config: failed to read " + configFile, e);
        }
        try {
            savedText = ConfigFiles.readOrNull(savedFile);
        } catch (IOException | RuntimeException e) {
            AgentLog.error("config: failed to read " + savedFile, e);
        }
        current = resolve(userText, savedText, null);
        lastUserText = userText;
        return current.config;
    }

    /**
     * The real server JVM, before the first connect: write the shipped template when there
     * is no config file and no token anywhere, generate and persist an identity when no
     * source holds one, put a saved identity back into a replaced config file, and bring
     * the saved copy up to date.
     */
    public synchronized ConfigFiles.Resolved prepareForStart() {
        String userText;
        String savedText;
        try {
            userText = ConfigFiles.readOrNull(configFile);
            savedText = ConfigFiles.readOrNull(savedFile);
        } catch (IOException | RuntimeException e) {
            // Unreadable is not absent: writing a template now could replace a real file.
            AgentLog.error("config: failed to read the config files; using what premain loaded", e);
            return current != null ? current : resolve(null, null, null);
        }

        ConfigFiles.Resolved probe = resolve(userText, savedText, null);
        if (userText == null && !TakaroConfig.isSet(probe.config.getRegistrationToken())) {
            try {
                String template = ConfigFiles.template();
                ConfigFiles.writeAtomically(configFile, template, false);
                userText = template;
                AgentLog.log("config: created " + configFile + " (paste the registration token into it)");
            } catch (IOException | RuntimeException e) {
                AgentLog.error("config: could not create " + configFile, e);
            }
        }

        ConfigFiles.Resolved resolved = resolve(userText, savedText, newIdentity);
        if (resolved.identityGenerated) {
            AgentLog.log("config: no identityToken yet; generated a new one for this server");
        }

        Map<String, String> user = ConfigFiles.parse(userText);
        boolean envIdentity = TakaroConfig.isSet(env.get("TAKARO_IDENTITY_TOKEN"));
        if (userText != null && !envIdentity && resolved.fileIdentityToken != null
                && !TakaroConfig.isSet(user.get("identityToken"))) {
            try {
                String updated = ConfigFiles.withIdentity(userText, resolved.fileIdentityToken);
                ConfigFiles.writeAtomically(configFile, updated, false);
                userText = updated;
                AgentLog.log("config: wrote identityToken into " + configFile);
            } catch (IOException | RuntimeException e) {
                AgentLog.error("config: could not write identityToken into " + configFile, e);
            }
        }

        writeSavedCopy(resolved, savedText);
        current = resolved;
        lastUserText = userText;
        return resolved;
    }

    /**
     * One watcher tick. Returns the new settings when the config file's text changed and
     * a second read shortly after saw the same text; null otherwise (unchanged, mid-save
     * or unreadable: the current settings stay and the next tick looks again).
     */
    public synchronized ConfigFiles.Resolved poll() {
        String text;
        try {
            text = ConfigFiles.readOrNull(configFile);
            if (Objects.equals(text, lastUserText)) {
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

        String savedText;
        try {
            savedText = ConfigFiles.readOrNull(savedFile);
        } catch (IOException | RuntimeException e) {
            savedText = null;
        }
        String keepIdentity = current != null ? current.fileIdentityToken : null;
        ConfigFiles.Resolved resolved = resolve(text, savedText, () -> keepIdentity);
        AgentLog.log("config: " + configFile + " changed");
        writeSavedCopy(resolved, savedText);
        current = resolved;
        lastUserText = text;
        return resolved;
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

    private ConfigFiles.Resolved resolve(String userText, String savedText, Supplier<String> identity) {
        ConfigFiles.Resolved resolved = ConfigFiles.resolve(
                ConfigFiles.parse(userText), ConfigFiles.parse(savedText), env, identity);
        resolved.config.setConfigFileHint(configFile.toAbsolutePath().toString());
        return resolved;
    }

    private void writeSavedCopy(ConfigFiles.Resolved resolved, String savedText) {
        String desired = ConfigFiles.renderSaved(resolved);
        if (desired == null) {
            return;
        }
        Map<String, String> have = ConfigFiles.parse(savedText);
        Map<String, String> want = ConfigFiles.parse(desired);
        if (savedText != null && have.equals(want)) {
            return;
        }
        try {
            ConfigFiles.writeAtomically(savedFile, desired, true);
            AgentLog.log("config: saved the tokens in use to " + savedFile);
        } catch (IOException | RuntimeException e) {
            AgentLog.error("config: could not write " + savedFile, e);
        }
    }
}
