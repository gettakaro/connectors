package io.takaro.zomboid.agent;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Does this agent belong in this server?
 *
 * <p>The agent's hooks are pinned to one Project Zomboid build's bytecode signatures. Run
 * against another build they can bind nothing, or — worse — bind the wrong method and
 * corrupt the game's state silently. So the first thing {@code premain} does is compare
 * the server jar the JVM actually loaded with the one the catalog target pins, and say so
 * in one machine-readable line.
 *
 * <p>{@code TAKARO_TARGET_POLICY} decides what happens on a mismatch: {@code enforce}
 * (the default) installs nothing, {@code warn} continues after saying so, {@code off}
 * does not even hash. Two outcomes are never a refusal: a jar built without a target
 * ({@code unpinned}, a developer's own build) and a JVM with no game jar on its class
 * path ({@code no-game-jar} — {@code premain} runs in the launcher's probe JVMs too,
 * three times per boot, and only the last one loads the game).
 */
public final class TargetGuard {

    public static final String TARGET_INFO_RESOURCE = "/META-INF/takaro-target.json";
    public static final String POLICY_ENV = "TAKARO_TARGET_POLICY";
    public static final String GAME_JAR_NAME = "projectzomboid.jar";

    /** What the guard decided, and whether premain may go on to install the hooks. */
    public record Decision(String result, boolean proceed, List<String> reasons) {
    }

    static final class UnreadableJar extends RuntimeException {
        UnreadableJar(Path path, Throwable cause) {
            super("the server jar " + path + " could not be hashed (" + cause + ")", cause);
        }
    }

    private TargetGuard() {
    }

    /**
     * The whole decision, as a pure function of what was observed. Kept separate from the
     * reading and the hashing so it can be tested without a JVM that has a game in it.
     */
    public static Decision decide(String policy, String expectedSha, String actualSha,
            boolean hasTargetInfo, boolean hasGameJar, boolean hashFailed) {
        return decide(policy, expectedSha, actualSha, hasTargetInfo, hasGameJar,
                hashFailed, hashFailed ? "cause unavailable" : null);
    }

    private static Decision decide(String policy, String expectedSha, String actualSha,
            boolean hasTargetInfo, boolean hasGameJar, boolean hashFailed, String hashFailure) {
        String mode = normalisePolicy(policy);
        List<String> reasons = new ArrayList<>();
        if (!hasTargetInfo) {
            reasons.add("this agent jar carries no " + TARGET_INFO_RESOURCE + "; it was not built for a target");
            return new Decision("unpinned", true, reasons);
        }
        if ("off".equals(mode)) {
            reasons.add(POLICY_ENV + "=off: the server jar was not checked");
            return new Decision("skipped", true, reasons);
        }
        if (expectedSha == null || expectedSha.isEmpty()) {
            reasons.add("the target carries no gameJarSha256 to check against");
            return new Decision("unpinned", true, reasons);
        }
        if (!hasGameJar) {
            reasons.add("no " + GAME_JAR_NAME + " on this JVM's class path (a launcher probe JVM)");
            return new Decision("no-game-jar", true, reasons);
        }
        if (hashFailed || actualSha == null || actualSha.isEmpty()) {
            reasons.add("the server jar " + GAME_JAR_NAME + " could not be hashed ("
                    + (hashFailure == null ? "cause unavailable" : hashFailure) + ")");
            if ("warn".equals(mode)) {
                reasons.add(POLICY_ENV + "=warn: continuing anyway; the hooks may bind nothing");
                return new Decision("unreadable", true, reasons);
            }
            reasons.add("set " + POLICY_ENV + "=warn to run it anyway");
            return new Decision("unreadable", false, reasons);
        }
        if (expectedSha.equals(actualSha)) {
            return new Decision("ok", true, reasons);
        }
        reasons.add("the server jar hashes " + actualSha + ", this build is pinned to " + expectedSha);
        if ("warn".equals(mode)) {
            reasons.add(POLICY_ENV + "=warn: continuing anyway; the hooks may bind nothing");
            return new Decision("refuse", true, reasons);
        }
        reasons.add("set " + POLICY_ENV + "=warn to run it anyway, or deploy the agent built for this build");
        return new Decision("refuse", false, reasons);
    }

    private static String normalisePolicy(String policy) {
        if (policy == null) {
            return "enforce";
        }
        String mode = policy.trim().toLowerCase(Locale.ROOT);
        return switch (mode) {
            case "warn", "off" -> mode;
            default -> "enforce";
        };
    }

    /** Run the check, log exactly one line about it, and say whether premain may go on. */
    public static boolean check() {
        String policy = normalisePolicy(System.getenv(POLICY_ENV));
        JsonObject info = readTargetInfo();
        boolean hasTargetInfo = info != null;
        JsonObject constraints = hasTargetInfo && info.has("constraints") && info.get("constraints").isJsonObject()
                ? info.getAsJsonObject("constraints")
                : new JsonObject();
        String expectedSha = string(constraints, "gameJarSha256");
        String gameVersion = string(constraints, "gameVersion");

        Path gameJar = "off".equals(policy) ? null : findGameJar();
        String actualSha = null;
        boolean hashFailed = false;
        String hashFailure = null;
        if (gameJar != null) {
            try {
                actualSha = sha256(gameJar);
            } catch (UnreadableJar e) {
                hashFailed = true;
                hashFailure = e.getCause() == null ? e.toString() : e.getCause().toString();
            }
        }
        Decision decision = decide(
                policy, expectedSha, actualSha, hasTargetInfo, gameJar != null, hashFailed, hashFailure);

        JsonObject expected = new JsonObject();
        expected.addProperty("gameJarSha256", expectedSha);
        expected.addProperty("gameVersion", gameVersion);
        JsonObject runtime = new JsonObject();
        runtime.addProperty("gameJar", gameJar == null ? null : gameJar.toString());
        runtime.addProperty("gameJarSha256", actualSha);
        runtime.addProperty("java", System.getProperty("java.version"));
        runtime.addProperty("vendor", System.getProperty("java.vm.vendor"));
        runtime.addProperty("agentVersion", hasTargetInfo ? string(info, "connectorVersion") : null);

        JsonObject line = new JsonObject();
        line.addProperty("result", decision.result());
        line.addProperty("policy", policy);
        line.addProperty("target", hasTargetInfo ? string(info, "target") : null);
        line.addProperty("fingerprint", hasTargetInfo ? string(info, "fingerprint") : null);
        line.add("expected", expected);
        line.add("runtime", runtime);
        Gson gson = new GsonBuilder().serializeNulls().create();
        line.add("reasons", gson.toJsonTree(decision.reasons()));
        AgentLog.log("target-check: " + gson.toJson(line));

        if (!decision.proceed()) {
            AgentLog.log("REFUSING to install hooks: this agent is built for another Project Zomboid build.");
        }
        return decision.proceed();
    }

    private static String string(JsonObject object, String member) {
        return object != null && object.has(member) && object.get(member).isJsonPrimitive()
                ? object.get(member).getAsString()
                : null;
    }

    private static JsonObject readTargetInfo() {
        try (InputStream stream = TargetGuard.class.getResourceAsStream(TARGET_INFO_RESOURCE)) {
            if (stream == null) {
                return null;
            }
            return new Gson().fromJson(new InputStreamReader(stream, StandardCharsets.UTF_8), JsonObject.class);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /**
     * The game jar on this JVM's class path. The launcher's class path is
     * {@code java/.:java/projectzomboid.jar}, so the entry is found by name rather than by
     * guessing at the install directory.
     */
    static Path findGameJar() {
        String classPath = System.getProperty("java.class.path", "");
        for (String entry : classPath.split(File.pathSeparator)) {
            if (entry.isEmpty()) {
                continue;
            }
            try {
                Path path = Paths.get(entry);
                if (path.getFileName() != null
                        && GAME_JAR_NAME.equals(path.getFileName().toString())
                        && Files.isRegularFile(path)) {
                    return path;
                }
            } catch (RuntimeException ignored) {
                // A class-path entry that is not a usable path is simply not the game jar.
            }
        }
        return null;
    }

    static String sha256(Path path) {
        try (InputStream stream = Files.newInputStream(path)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1 << 16];
            int read;
            while ((read = stream.read(buffer)) > 0) {
                digest.update(buffer, 0, read);
            }
            StringBuilder out = new StringBuilder(64);
            for (byte b : digest.digest()) {
                out.append(Character.forDigit((b >> 4) & 0xf, 16)).append(Character.forDigit(b & 0xf, 16));
            }
            return out.toString();
        } catch (Exception e) {
            throw new UnreadableJar(path, e);
        }
    }
}
