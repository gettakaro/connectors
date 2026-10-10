package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

import static org.junit.jupiter.api.Assertions.*;

class ConfigLoaderTest {

    private final AtomicInteger generated = new AtomicInteger();
    private final Supplier<String> ids = () -> "generated-" + generated.incrementAndGet();

    private ConfigLoader loader(Path dir, Map<String, String> env) {
        return new ConfigLoader(env, ids, dir.resolve(TakaroPaths.CONFIG_NAME), dir.resolve(TakaroPaths.SAVED_NAME));
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
        assertFalse(Files.exists(dir.resolve("TakaroConfig.saved.txt")));
        assertEquals(0, generated.get());
    }

    @Test
    void freshInstallWritesTemplateAndGeneratesOneIdentity(@TempDir Path dir) throws Exception {
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertTrue(r.identityGenerated);
        assertEquals("generated-1", r.config.getIdentityToken());
        assertNull(r.config.getRegistrationToken());
        Path user = dir.resolve("TakaroConfig.txt");
        assertTrue(Files.readString(user).contains("Paste the registration token"), "template written");
        assertEquals("generated-1", parsed(user).get("identityToken"));
        assertEquals("", parsed(user).get("registrationToken"));
        assertEquals("generated-1", parsed(dir.resolve("TakaroConfig.saved.txt")).get("identityToken"));
        assertEquals(dir.resolve("TakaroConfig.txt").toAbsolutePath().toString(), r.config.getConfigFileHint());

        // A restart reads the identity back instead of making up another one.
        ConfigLoader again = loader(dir, Map.of());
        again.load();
        assertEquals("generated-1", again.prepareForStart().config.getIdentityToken());
        assertEquals(1, generated.get());
    }

    @Test
    void existingInstallKeepsItsIdentityAndGetsASavedCopy(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        String original = "wsUrl=wss://connect.takaro.io/\nregistrationToken=reg-1\nidentityToken=my-server\n";
        Files.writeString(user, original);
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertFalse(r.identityGenerated);
        assertEquals("my-server", r.config.getIdentityToken());
        assertEquals("reg-1", r.config.getRegistrationToken());
        assertEquals(original, Files.readString(user), "the user file is left alone");
        Map<String, String> saved = parsed(dir.resolve("TakaroConfig.saved.txt"));
        assertEquals("reg-1", saved.get("registrationToken"));
        assertEquals("my-server", saved.get("identityToken"));
        assertEquals(0, generated.get());
    }

    @Test
    void upgradeThatReplacesTheConfigKeepsTokenAndIdentity(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader first = loader(dir, Map.of());
        first.load();
        first.prepareForStart();

        // The README's upgrade: unpack the zip again, which puts the empty template back.
        Files.writeString(user, ConfigFiles.template());
        ConfigLoader after = loader(dir, Map.of());
        after.load();
        ConfigFiles.Resolved r = after.prepareForStart();

        assertEquals("reg-1", r.config.getRegistrationToken());
        assertEquals("my-server", r.config.getIdentityToken());
        assertFalse(r.identityGenerated);
        Map<String, String> userValues = parsed(user);
        assertEquals("my-server", userValues.get("identityToken"), "identity written back");
        assertEquals("", userValues.get("registrationToken"), "the token is never copied into the user file");
        assertEquals(0, generated.get());
    }

    @Test
    void tokenInTheUserFileWinsOverTheSavedCopy(@TempDir Path dir) throws Exception {
        Files.writeString(dir.resolve("TakaroConfig.saved.txt"), "registrationToken=old\nidentityToken=my-server\n");
        Files.writeString(dir.resolve("TakaroConfig.txt"), "registrationToken=new\nidentityToken=\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("new", r.config.getRegistrationToken());
        assertEquals("my-server", r.config.getIdentityToken());
        assertEquals("new", parsed(dir.resolve("TakaroConfig.saved.txt")).get("registrationToken"));
    }

    @Test
    void environmentWinsAndIsNeverWrittenToDisk(@TempDir Path dir) throws Exception {
        Map<String, String> env = new HashMap<>();
        env.put("TAKARO_REGISTRATION_TOKEN", "env-reg");
        env.put("TAKARO_IDENTITY_TOKEN", "env-id");
        env.put("TAKARO_WS_URL", "wss://env.example/");
        env.put("TAKARO_DEBUG", "true");
        env.put("TAKARO_LOG_EVENTS", "true");
        Files.writeString(dir.resolve("TakaroConfig.txt"),
                "wsUrl=wss://file.example/\nregistrationToken=file-reg\nidentityToken=file-id\ndebug=false\n");
        ConfigLoader loader = loader(dir, env);
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("env-reg", r.config.getRegistrationToken());
        assertEquals("env-id", r.config.getIdentityToken());
        assertEquals("wss://env.example/", r.config.getWsUrl());
        assertTrue(r.config.isDebugEnabled());
        assertTrue(r.logEvents);
        String saved = Files.readString(dir.resolve("TakaroConfig.saved.txt"));
        assertFalse(saved.contains("env-"), "only file-sourced tokens are saved");
    }

    @Test
    void dockerStyleEnvOnlyInstallGetsNoTemplate(@TempDir Path dir) throws Exception {
        Map<String, String> env = Map.of("TAKARO_REGISTRATION_TOKEN", "env-reg", "TAKARO_IDENTITY_TOKEN", "env-id");
        ConfigLoader loader = loader(dir, env);
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();

        assertEquals("env-id", r.config.getIdentityToken());
        assertFalse(Files.exists(dir.resolve("TakaroConfig.txt")));
        assertFalse(Files.exists(dir.resolve("TakaroConfig.saved.txt")));
        assertEquals(0, generated.get());
    }

    @Test
    void envRegistrationWithoutIdentityPersistsAGeneratedOne(@TempDir Path dir) throws Exception {
        ConfigLoader loader = loader(dir, Map.of("TAKARO_REGISTRATION_TOKEN", "env-reg"));
        loader.load();
        ConfigFiles.Resolved r = loader.prepareForStart();
        assertEquals("generated-1", r.config.getIdentityToken());
        Map<String, String> saved = parsed(dir.resolve("TakaroConfig.saved.txt"));
        assertEquals("generated-1", saved.get("identityToken"));
        assertEquals("", saved.get("registrationToken"));

        ConfigLoader again = loader(dir, Map.of("TAKARO_REGISTRATION_TOKEN", "env-reg"));
        again.load();
        assertEquals("generated-1", again.prepareForStart().config.getIdentityToken());
    }

    @Test
    void pollAppliesASavedChangeOnceAndIgnoresAnUnchangedFile(@TempDir Path dir) throws Exception {
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();
        assertNull(loader.poll(), "nothing changed since start");

        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, Files.readString(user).replace("registrationToken=", "registrationToken=pasted"));
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertEquals("pasted", r.config.getRegistrationToken());
        assertEquals("generated-1", r.config.getIdentityToken(), "identity unchanged by a reload");
        assertEquals("pasted", parsed(dir.resolve("TakaroConfig.saved.txt")).get("registrationToken"));
        assertNull(loader.poll(), "the same text is not applied twice");
        assertEquals(1, generated.get());
    }

    @Test
    void pollWaitsWhileTheFileIsStillBeingWritten(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        loader.prepareForStart();

        Thread writer = new Thread(() -> {
            try {
                Files.writeString(user, "registrationToken=re");
                Thread.sleep(ConfigLoader.SETTLE_MILLIS / 3);
                Files.writeString(user, "registrationToken=reg-2\nidentityToken=my-server\n");
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
    void deletedOrHalfWrittenFileFallsBackToTheSavedTokens(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, "registrationToken=reg-1\nidentityToken=my-server\n");
        ConfigLoader loader = loader(dir, Map.of());
        loader.load();
        TakaroConfig before = loader.prepareForStart().config;

        Files.writeString(user, "registrationTo");
        ConfigFiles.Resolved r = loader.poll();
        assertNotNull(r);
        assertTrue(before.sameConnection(r.config), "a truncated file keeps the same connection");

        Files.delete(user);
        r = loader.poll();
        assertNotNull(r);
        assertTrue(before.sameConnection(r.config), "a deleted file keeps the same connection");
    }

    @Test
    void placeholderTokenFromTheOldReadmeCountsAsUnset(@TempDir Path dir) throws Exception {
        Files.writeString(dir.resolve("TakaroConfig.txt"), "registrationToken=your-registration-token-here\n");
        ConfigLoader loader = loader(dir, Map.of());
        assertNull(loader.load().getRegistrationToken());
    }
}
