package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFileAttributeView;
import java.nio.file.attribute.PosixFileAttributes;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.function.Supplier;

/**
 * Reading, merging and writing the two config files, with no game types and no global
 * state so the tests can drive every case.
 *
 * <p>{@code TakaroConfig.txt} ships in the release zip and is the file people edit. An
 * upgrade that unpacks the zip again replaces it with the empty template, so the
 * connector keeps the tokens in use in {@code TakaroConfig.saved.txt} beside it (a file
 * the zip does not contain) and falls back to it per field.
 */
public final class ConfigFiles {

    public static final String TEMPLATE_RESOURCE = "/io/takaro/zomboid/agent/TakaroConfig.txt";

    private static final String SAVED_HEADER = String.join("\n",
            "# Written by the Takaro connector: the tokens it uses, kept so that an upgrade",
            "# that replaces TakaroConfig.txt keeps them. Edit TakaroConfig.txt instead; a",
            "# token set there takes precedence over this file.",
            "");

    private ConfigFiles() {
    }

    /** The settings a resolution produced, and whether it had to make up the identity. */
    public static final class Resolved {
        public final TakaroConfig config;
        public final boolean logEvents;
        public final boolean debugCatalog;
        /** The tokens that came from the files (or were generated), never from the environment. */
        public final String fileRegistrationToken;
        public final String fileIdentityToken;
        public final boolean identityGenerated;

        Resolved(TakaroConfig config, boolean logEvents, boolean debugCatalog,
                 String fileRegistrationToken, String fileIdentityToken, boolean identityGenerated) {
            this.config = config;
            this.logEvents = logEvents;
            this.debugCatalog = debugCatalog;
            this.fileRegistrationToken = fileRegistrationToken;
            this.fileIdentityToken = fileIdentityToken;
            this.identityGenerated = identityGenerated;
        }
    }

    /** {@code key=value} lines; {@code #} starts a comment; unknown keys are kept and ignored. */
    public static Map<String, String> parse(String text) {
        Map<String, String> values = new LinkedHashMap<>();
        if (text == null) {
            return values;
        }
        for (String raw : text.split("\\R", -1)) {
            String line = stripComment(raw).trim();
            int eq = line.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            values.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        return values;
    }

    /**
     * The settings to use. Each token comes from the environment, else the user file, else
     * the saved copy; when no source holds an identity, {@code newIdentity} supplies one
     * (null leaves it unset). Every other setting comes from the environment or the user
     * file only.
     */
    public static Resolved resolve(Map<String, String> user, Map<String, String> saved,
                                   Map<String, String> env, Supplier<String> newIdentity) {
        Map<String, String> u = user != null ? user : Map.of();
        Map<String, String> s = saved != null ? saved : Map.of();

        String fileRegistration = first(unlessPlaceholder(u.get("registrationToken")), s.get("registrationToken"));
        String fileIdentity = first(u.get("identityToken"), s.get("identityToken"));
        boolean generated = false;
        if (fileIdentity == null && !TakaroConfig.isSet(env.get("TAKARO_IDENTITY_TOKEN")) && newIdentity != null) {
            fileIdentity = newIdentity.get();
            generated = fileIdentity != null;
        }

        TakaroConfig config = new TakaroConfig();
        config.setWsUrl(first(u.get("wsUrl"), TakaroConfig.DEFAULT_WS_URL));
        config.setRegistrationToken(fileRegistration);
        config.setIdentityToken(fileIdentity);
        config.setServerChatName(first(u.get("serverChatName")));
        config.setDebugEnabled(isTruthy(u.get("debug")));
        config.applyEnvOverrides(env);

        boolean logEvents = isTruthy(u.get("logEvents"));
        String logEventsEnv = env.get("TAKARO_LOG_EVENTS");
        if (TakaroConfig.isSet(logEventsEnv)) {
            logEvents = isTruthy(logEventsEnv);
        }
        boolean debugCatalog = isTruthy(u.get("debugCatalog"));
        String debugCatalogEnv = env.get("TAKARO_DEBUG_CATALOG");
        if (TakaroConfig.isSet(debugCatalogEnv)) {
            debugCatalog = isTruthy(debugCatalogEnv);
        }
        return new Resolved(config, logEvents, debugCatalog, fileRegistration, fileIdentity, generated);
    }

    /** What the saved copy should hold, or null when there is nothing worth keeping. */
    public static String renderSaved(Resolved resolved) {
        if (resolved.fileRegistrationToken == null && resolved.fileIdentityToken == null) {
            return null;
        }
        return SAVED_HEADER
                + "registrationToken=" + nullToEmpty(resolved.fileRegistrationToken) + "\n"
                + "identityToken=" + nullToEmpty(resolved.fileIdentityToken) + "\n";
    }

    /**
     * The user file's text with {@code identityToken} set: the first such line is
     * rewritten in place (its comments and the rest of the file untouched), or a line is
     * appended when the file has none.
     */
    public static String withIdentity(String text, String identity) {
        String source = text != null ? text : "";
        String[] lines = source.split("\\R", -1);
        StringBuilder out = new StringBuilder();
        boolean replaced = false;
        for (int i = 0; i < lines.length; i++) {
            String line = lines[i];
            String code = stripComment(line);
            int eq = code.indexOf('=');
            if (!replaced && eq > 0 && code.substring(0, eq).trim().equals("identityToken")) {
                int hash = line.indexOf('#');
                String comment = hash >= 0 ? " " + line.substring(hash) : "";
                line = line.substring(0, line.indexOf('=') + 1) + identity + comment;
                replaced = true;
            }
            out.append(line);
            if (i < lines.length - 1) {
                out.append('\n');
            }
        }
        if (!replaced) {
            if (out.length() > 0 && out.charAt(out.length() - 1) != '\n') {
                out.append('\n');
            }
            out.append("identityToken=").append(identity).append('\n');
        }
        return out.toString();
    }

    /** The template the release ships, as bundled in the jar. */
    public static String template() throws IOException {
        try (InputStream in = ConfigFiles.class.getResourceAsStream(TEMPLATE_RESOURCE)) {
            if (in == null) {
                throw new IOException("template " + TEMPLATE_RESOURCE + " missing from the jar");
            }
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    /** Null when the file does not exist. */
    public static String readOrNull(Path file) throws IOException {
        if (!Files.exists(file)) {
            return null;
        }
        return Files.readString(file, StandardCharsets.UTF_8);
    }

    /** A reader polling the file never sees it half-written; the saved copy is owner-only. */
    public static void writeAtomically(Path file, String text, boolean ownerOnly) throws IOException {
        Path parent = file.toAbsolutePath().getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        Path tmp = file.resolveSibling(file.getFileName() + ".tmp");
        Files.writeString(tmp, text, StandardCharsets.UTF_8);
        matchOwner(tmp, file, ownerOnly);
        try {
            Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException e) {
            Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    /**
     * The file keeps the owner and mode it had, and a new file takes its folder's owner:
     * a server running as root (a Docker image, say) must not leave the config behind as a
     * file the owner of the folder can no longer edit.
     */
    private static void matchOwner(Path tmp, Path file, boolean ownerOnly) {
        try {
            PosixFileAttributeView view = Files.getFileAttributeView(tmp, PosixFileAttributeView.class);
            if (view == null) {
                return; // not a POSIX file system (Windows): the folder's own ACL applies
            }
            boolean exists = Files.exists(file);
            Path reference = exists ? file : file.toAbsolutePath().getParent();
            if (reference == null) {
                return;
            }
            PosixFileAttributes wanted = Files.readAttributes(reference, PosixFileAttributes.class);
            Set<PosixFilePermission> mode = ownerOnly
                    ? PosixFilePermissions.fromString("rw-------")
                    : exists ? wanted.permissions() : PosixFilePermissions.fromString("rw-r--r--");
            view.setPermissions(mode);
            PosixFileAttributes have = view.readAttributes();
            if (!have.owner().equals(wanted.owner())) {
                try {
                    view.setOwner(wanted.owner());
                } catch (IOException ignored) {
                    // only root can give a file away; then it stays ours
                }
            }
            if (!have.group().equals(wanted.group())) {
                try {
                    view.setGroup(wanted.group());
                } catch (IOException ignored) {
                    // not a member of that group
                }
            }
        } catch (IOException | UnsupportedOperationException ignored) {
            // best effort: the write itself already succeeded
        }
    }

    static boolean isTruthy(String v) {
        return "true".equalsIgnoreCase(v) || "1".equals(v) || "yes".equalsIgnoreCase(v);
    }

    private static String first(String... values) {
        for (String v : values) {
            if (TakaroConfig.isSet(v)) {
                return v;
            }
        }
        return null;
    }

    /** The value the README's example config used to show; never a real token. */
    static final String PLACEHOLDER_REGISTRATION_TOKEN = "your-registration-token-here";

    private static String unlessPlaceholder(String v) {
        return PLACEHOLDER_REGISTRATION_TOKEN.equals(v) ? null : v;
    }

    private static String nullToEmpty(String v) {
        return v != null ? v : "";
    }

    private static String stripComment(String line) {
        int hash = line.indexOf('#');
        return hash >= 0 ? line.substring(0, hash) : line;
    }
}
