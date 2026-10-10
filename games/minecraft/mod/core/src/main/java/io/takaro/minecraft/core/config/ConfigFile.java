package io.takaro.minecraft.core.config;

import io.takaro.minecraft.core.GameAdapter;
import io.takaro.minecraft.core.TakaroConfig;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.UUID;
import java.util.function.Function;
import java.util.function.Supplier;

/**
 * The connector's config file, read at start and re-read by the watcher so a pasted or
 * corrected token takes effect without a restart.
 *
 * <p>The text is compared, not the modification time: a save within the same second, or one
 * that restores the old size, must still count. An identity is generated only when neither the
 * file nor TAKARO_IDENTITY_TOKEN has one; Takaro rejects an empty identity, so no install that
 * ever connected can be running without one.
 */
public final class ConfigFile {

    private final Path path;
    private final ConfigFormat format;
    private final GameAdapter log;
    private final Function<String, String> env;
    private final Supplier<String> identityGenerator;

    // A save is read twice this far apart and only applied when both reads agree, so a
    // token that is still being written is not mistaken for a new one.
    private final long settleMillis;

    private String lastText;
    // The identity this run uses. A file that loses it (an editor saving a stale copy) gets
    // it back instead of a new one, which would make Takaro see a different server.
    private String identity;
    private boolean identityUnsaved;

    public ConfigFile(Path path, ConfigFormat format, GameAdapter log) {
        this(path, format, log, System::getenv, () -> UUID.randomUUID().toString(), 300);
    }

    public ConfigFile(Path path, ConfigFormat format, GameAdapter log,
                      Function<String, String> env, Supplier<String> identityGenerator) {
        this(path, format, log, env, identityGenerator, 0);
    }

    ConfigFile(Path path, ConfigFormat format, GameAdapter log, Function<String, String> env,
               Supplier<String> identityGenerator, long settleMillis) {
        this.settleMillis = settleMillis;
        this.path = path.toAbsolutePath().normalize();
        this.format = format;
        this.log = log;
        this.env = env;
        this.identityGenerator = identityGenerator;
    }

    public Path path() {
        return path;
    }

    /**
     * The settings to start with. Creates the file when it is missing. Never null: an
     * unreadable file yields the defaults (no token, so no connection) until it is fixed.
     */
    public synchronized TakaroConfig load() {
        if (!Files.exists(path)) {
            try {
                Path parent = path.getParent();
                if (parent != null) {
                    Files.createDirectories(parent);
                }
                Files.writeString(path, format.defaultText(), StandardCharsets.UTF_8);
                log.logInfo("Created the Takaro config file " + path);
            } catch (IOException e) {
                log.logWarning("Could not create " + path + ": " + e.getMessage());
            }
        }
        String text = read();
        TakaroConfig resolved = text == null ? null : resolve(text);
        if (resolved == null) {
            resolved = effective(new TakaroConfig());
        }
        return resolved;
    }

    /** The new settings when the file changed and reads cleanly, else null. */
    public synchronized TakaroConfig poll() {
        String text = read();
        if (text == null) {
            return null;
        }
        if (text.equals(lastText)) {
            if (identityUnsaved) {
                saveIdentity(text);
            }
            return null;
        }
        if (settleMillis > 0) {
            try {
                Thread.sleep(settleMillis);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
            if (!text.equals(read())) {
                return null; // still being written; the next tick sees the finished save
            }
        }
        return resolve(text);
    }

    private String read() {
        try {
            return Files.readString(path, StandardCharsets.UTF_8);
        } catch (IOException e) {
            // A file being replaced by an editor can be missing for a moment.
            return null;
        }
    }

    private TakaroConfig resolve(String text) {
        lastText = text;
        TakaroConfig file;
        try {
            file = format.parse(text);
        } catch (Exception e) {
            log.logWarning("Could not read " + path + " (" + e.getMessage()
                    + "). Keeping the current settings; fix the file and save it again.");
            return null;
        }

        String fileIdentity = trimToNull(file.getIdentityToken());
        if (fileIdentity != null) {
            identity = fileIdentity;
            identityUnsaved = false;
        } else if (trimToNull(env.apply("TAKARO_IDENTITY_TOKEN")) == null) {
            if (identity == null) {
                identity = identityGenerator.get();
                log.logInfo("No identity token set; generated one for this server and saving it in " + path);
            } else {
                log.logWarning("identity_token was removed from " + path + "; putting this server's identity back");
            }
            file.setIdentityToken(identity);
            saveIdentity(text);
        } else {
            identityUnsaved = false;
        }
        return effective(file);
    }

    private void saveIdentity(String text) {
        try {
            // Someone saved since we read it: leave their save alone and retry on the next tick.
            if (!text.equals(read())) {
                identityUnsaved = true;
                return;
            }
            String updated = format.withIdentity(text, identity);
            write(updated);
            lastText = updated;
            identityUnsaved = false;
        } catch (Exception e) {
            identityUnsaved = true;
            log.logWarning("Could not save the identity token in " + path + " (" + e.getMessage()
                    + "); using it for now and trying again.");
        }
    }

    /** Replace the file in one step where the filesystem allows it, so a crash never leaves half a config. */
    private void write(String text) throws IOException {
        if (Files.isSymbolicLink(path)) {
            Files.writeString(path, text, StandardCharsets.UTF_8);
            return;
        }
        Path tmp = path.resolveSibling(path.getFileName() + ".tmp");
        Files.writeString(tmp, text, StandardCharsets.UTF_8);
        try {
            Files.move(tmp, path, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException atomicFailed) {
            Files.move(tmp, path, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private TakaroConfig effective(TakaroConfig file) {
        file.setWsUrl(trimToNull(file.getWsUrl()) == null ? TakaroConfig.DEFAULT_WS_URL : file.getWsUrl().trim());
        file.setIdentityToken(trimToEmpty(file.getIdentityToken()));
        file.setRegistrationToken(trimToEmpty(file.getRegistrationToken()));
        file.applyEnvOverrides(env);
        return file;
    }

    private static String trimToNull(String s) {
        if (s == null) return null;
        String t = s.trim();
        return t.isEmpty() ? null : t;
    }

    private static String trimToEmpty(String s) {
        return s == null ? "" : s.trim();
    }
}
