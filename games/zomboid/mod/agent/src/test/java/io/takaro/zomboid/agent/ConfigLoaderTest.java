package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

import static org.junit.jupiter.api.Assertions.*;

class ConfigLoaderTest {

    private final AtomicInteger generated = new AtomicInteger();
    private final Supplier<String> ids = () -> "generated-" + generated.incrementAndGet();

    private ConfigLoader loader(Path dir, Map<String, String> env) {
        return new ConfigLoader(env, ids, dir.resolve(TakaroPaths.CONFIG_NAME));
    }

    private static Map<String, String> parsed(Path file) throws Exception {
        return ConfigFiles.parse(Files.readString(file));
    }

    @Test
    void parsesKeyValueWithComments(@TempDir Path dir) throws Exception {
        Files.writeString(dir.resolve("TakaroConfig.txt"), String.join("\n",
                "# Takaro config",
                "wsUrl=wss://connect.example/   # trailing comment",
                "identityToken=my-identity",
                "registrationToken=my-registration",
                "debug=true",
                "logEvents=yes",
                "",
                "unknownKey=ignored"));
        ConfigLoader loader = loader(dir, Map.of());
        TakaroConfig config = loader.load();

        assertEquals("wss://connect.example/", config.getWsUrl());
        assertEquals("my-identity", config.getIdentityToken());
        assertEquals("my-registration", config.getRegistrationToken());
        assertTrue(config.isDebugEnabled());
        assertTrue(loader.isLogEvents());
    }

    @Test
    void premainLoadNeverWritesOrGenerates(@TempDir Path dir) {
        ConfigLoader loader = loader(dir, Map.of());
        TakaroConfig config = assertDoesNotThrow(loader::load);
        assertEquals(TakaroConfig.DEFAULT_WS_URL, config.getWsUrl());
        assertNull(config.getIdentityToken());
        assertNull(config.getRegistrationToken());
        assertFalse(loader.isLogEvents());
        assertFalse(Files.exists(dir.resolve("TakaroConfig.txt")));
        assertEquals(0, generated.get());
    }

    @Test
    void missingFileIsCreatedWithEveryKeyAndANewIdentity(@TempDir Path dir) throws Exception {
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("generated-1", r.config.getIdentityToken());
        assertNull(r.config.getRegistrationToken());
        Path file = dir.resolve("TakaroConfig.txt");
        Map<String, String> values = parsed(file);
        assertEquals("generated-1", values.get("identityToken"));
        assertEquals("", values.get("registrationToken"));
        assertEquals(TakaroConfig.DEFAULT_WS_URL, values.get("wsUrl"));
        assertTrue(values.keySet().containsAll(List.of(
                "serverChatName", "debug", "logEvents", "debugCatalog")));
        assertEquals(Files.getOwner(dir), Files.getOwner(file));
        assertEquals(file.toAbsolutePath().toString(), r.config.getConfigFileHint());

        // A restart reads the identity back instead of making up another one.
        ConfigLoader again = loader(dir, Map.of());
        again.load();
        assertEquals("generated-1", again.prepareForStart().config.getIdentityToken());
        assertEquals(1, generated.get());
    }

    @Test
    void aFileWithoutIdentityGetsOneAndKeepsItsModeAndTheRest(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, "# mine\nregistrationToken=reg-1\nidentityToken=\n");
        Files.setPosixFilePermissions(file, PosixFilePermissions.fromString("rw-rw----"));
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("generated-1", r.config.getIdentityToken());
        assertEquals("reg-1", r.config.getRegistrationToken());
        assertEquals("# mine\nregistrationToken=reg-1\nidentityToken=generated-1\n", Files.readString(file));
        assertEquals("rw-rw----", PosixFilePermissions.toString(Files.getPosixFilePermissions(file)));
    }

    @Test
    void aCompleteFileIsLeftAlone(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        String original = "wsUrl=wss://connect.takaro.io/\nregistrationToken=reg-1\nidentityToken=my-server\n";
        Files.writeString(file, original);
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("my-server", r.config.getIdentityToken());
        assertEquals(original, Files.readString(file));
        assertEquals(0, generated.get());
    }

    @Test
    void environmentWinsAndIsNeverWrittenToDisk(@TempDir Path dir) throws Exception {
        Map<String, String> env = new HashMap<>();
        env.put("TAKARO_REGISTRATION_TOKEN", "env-reg");
        env.put("TAKARO_IDENTITY_TOKEN", "env-id");
        env.put("TAKARO_WS_URL", "wss://env.example/");
        env.put("TAKARO_DEBUG", "true");
        env.put("TAKARO_LOG_EVENTS", "true");
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file,
                "wsUrl=wss://file.example/\nregistrationToken=file-reg\nidentityToken=file-id\ndebug=false\n");
        ConfigLoader loader = loader(dir, env);
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("env-reg", r.config.getRegistrationToken());
        assertEquals("env-id", r.config.getIdentityToken());
        assertEquals("wss://env.example/", r.config.getWsUrl());
        assertTrue(r.config.isDebugEnabled());
        assertTrue(r.logEvents);
        assertFalse(Files.readString(file).contains("env-"));

        Path freshDir = Files.createDirectory(dir.resolve("fresh"));
        ConfigLoader fresh = loader(freshDir, env);
        fresh.load();
        assertEquals("env-id", fresh.prepareForStart().config.getIdentityToken());
        assertFalse(Files.readString(freshDir.resolve("TakaroConfig.txt")).contains("env-"));
    }

    @Test
    void pollAppliesASavedChangeOnceAndIgnoresAnUnchangedFile(@TempDir Path dir) throws Exception {
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();
        assertNull(loader.poll(), "nothing changed since start");

        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, Files.readString(file).replace("registrationToken=", "registrationToken=pasted"));
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertEquals("pasted", r.config.getRegistrationToken());
        assertEquals("generated-1", r.config.getIdentityToken(), "identity unchanged by a reload");
        assertNull(loader.poll(), "the same text is not applied twice");
        assertEquals(1, generated.get());
    }

    @Test
    void pollWaitsWhileTheFileIsStillBeingWritten(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();

        Thread writer = new Thread(() -> {
            try {
                Files.writeString(file, "registrationToken=re");
                Thread.sleep(ConfigLoader.SETTLE_MILLIS / 3);
                Files.writeString(file, "registrationToken=reg-2\nidentityToken=my-server\n");
            } catch (Exception ignored) {
                // the assertion below reports it
            }
        });
        writer.start();
        Thread.sleep(20);
        assertNull(loader.poll(), "a file that changes between the two reads is not applied");
        writer.join();
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertEquals("reg-2", r.config.getRegistrationToken());
    }

    @Test
    void anEmptiedTokenIsApplied(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();

        Files.writeString(file, "registrationToken=\nidentityToken=my-server\n");
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertNull(r.config.getRegistrationToken());
        assertEquals("my-server", r.config.getIdentityToken());
    }

    @Test
    void aDeletedFileKeepsTheCurrentSettings(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();

        Files.delete(file);
        assertNull(loader.poll());
        assertFalse(Files.exists(file), "only a start creates the file");
    }

    @Test
    void anEmptiedIdentityIsPutBackSoTheNextStartKeepsTheRecord(@TempDir Path dir) throws Exception {
        Path file = dir.resolve("TakaroConfig.txt");
        Files.writeString(file, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();

        Files.writeString(file, "registrationToken=reg-2\nidentityToken=\n");
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertEquals("reg-2", r.config.getRegistrationToken());
        assertEquals("my-server", r.config.getIdentityToken());
        assertEquals("my-server", parsed(file).get("identityToken"));
        assertNull(loader.poll(), "its own write is not a change");
        assertEquals(0, generated.get());
    }
}
