package io.takaro.minecraft.core.config;

import io.takaro.minecraft.core.TakaroConfig;
import io.takaro.minecraft.core.TakaroConnectorTestSupport;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;

import static org.junit.jupiter.api.Assertions.*;

class ConfigFileTest {

    @TempDir
    Path dir;

    private final Map<String, String> env = new HashMap<>();
    private final TakaroConnectorTestSupport.LogAdapter log = new TakaroConnectorTestSupport.LogAdapter();
    private int generated;
    private final Supplier<String> ids = () -> "generated-" + (++generated);

    private ConfigFile file(Path path, ConfigFormat format) {
        return new ConfigFile(path, format, log, env::get, ids);
    }

    static List<ConfigFormat> formats() {
        return List.of(new JsonConfigFormat(), new PropertiesConfigFormat());
    }

    @Test
    void freshInstallWritesDefaultsGeneratesIdentityAndDefaultsTheUrl() throws Exception {
        for (ConfigFormat format : formats()) {
            Path path = dir.resolve("fresh-" + format.getClass().getSimpleName()).resolve("takaro.cfg");
            ConfigFile cf = file(path, format);

            TakaroConfig c = cf.load();

            assertTrue(Files.exists(path));
            assertEquals(TakaroConfig.DEFAULT_WS_URL, c.getWsUrl());
            assertEquals("", c.getRegistrationToken());
            assertTrue(c.getIdentityToken().startsWith("generated-"));
            // persisted, and read back the same
            assertEquals(c.getIdentityToken(), format.parse(Files.readString(path)).getIdentityToken());
            assertNull(cf.poll(), "writing the identity must not count as a user change");
        }
    }

    @Test
    void existingIdentityIsNeverReplaced() throws Exception {
        Path path = dir.resolve("takaro.properties");
        Files.writeString(path, "takaro.websocket.url=\ntakaro.authentication.identity_token=my-smp\n"
                + "takaro.authentication.registration_token=tok\n");
        TakaroConfig c = file(path, new PropertiesConfigFormat()).load();

        assertEquals("my-smp", c.getIdentityToken());
        assertEquals("tok", c.getRegistrationToken());
        assertEquals(TakaroConfig.DEFAULT_WS_URL, c.getWsUrl(), "pre-0.4 files have an empty url");
        assertEquals(0, generated);
        assertTrue(Files.readString(path).contains("identity_token=my-smp"));
    }

    @Test
    void envIdentityMeansNoGeneratedOne() throws Exception {
        env.put("TAKARO_IDENTITY_TOKEN", "takaro-dev-fabric");
        env.put("TAKARO_REGISTRATION_TOKEN", "env-token");
        Path path = dir.resolve("takaro.json");
        TakaroConfig c = file(path, new JsonConfigFormat()).load();

        assertEquals("takaro-dev-fabric", c.getIdentityToken());
        assertEquals("env-token", c.getRegistrationToken());
        assertEquals(0, generated);
        assertEquals("", new JsonConfigFormat().parse(Files.readString(path)).getIdentityToken(),
                "the file must stay untouched for docker installs");
    }

    @Test
    void envWinsOverTheFile() throws Exception {
        env.put("TAKARO_REGISTRATION_TOKEN", "env-token");
        env.put("TAKARO_WS_URL", "ws://env/");
        Path path = dir.resolve("takaro.properties");
        Files.writeString(path, "takaro.websocket.url=ws://file/\ntakaro.authentication.identity_token=id\n"
                + "takaro.authentication.registration_token=file-token\n");
        TakaroConfig c = file(path, new PropertiesConfigFormat()).load();
        assertEquals("env-token", c.getRegistrationToken());
        assertEquals("ws://env/", c.getWsUrl());
    }

    @Test
    void tokenPastedLaterIsPickedUpByPoll() throws Exception {
        Path path = dir.resolve("takaro.json");
        ConfigFile cf = file(path, new JsonConfigFormat());
        TakaroConfig first = cf.load();
        assertNull(cf.poll());

        String text = Files.readString(path).replace("\"registration_token\": \"\"", "\"registration_token\": \" pasted \"");
        Files.writeString(path, text);
        TakaroConfig next = cf.poll();

        assertNotNull(next);
        assertEquals("pasted", next.getRegistrationToken(), "pasted tokens are trimmed");
        assertEquals(first.getIdentityToken(), next.getIdentityToken());
        assertNull(cf.poll(), "unchanged text is not a change");
    }

    @Test
    void halfSavedFileKeepsTheCurrentSettings() throws Exception {
        for (ConfigFormat format : formats()) {
            Path path = dir.resolve("half-" + format.getClass().getSimpleName()).resolve("takaro.cfg");
            ConfigFile cf = file(path, format);
            cf.load();
            String full = Files.readString(path);

            Files.writeString(path, "");
            assertNull(cf.poll(), "empty file mid-save");
            Files.writeString(path, full.substring(0, full.indexOf("registration_token") - 3));
            assertNull(cf.poll(), "truncated file mid-save");
            assertTrue(log.warnings.stream().anyMatch(w -> w.contains("Keeping the current settings")));

            Files.writeString(path, full.replace("registration_token" + sep(format) + "\"\"", "registration_token" + sep(format) + "\"tok\"")
                    .replace("registration_token=\n", "registration_token=tok\n"));
            TakaroConfig next = cf.poll();
            assertNotNull(next, "the finished save is applied");
            assertEquals("tok", next.getRegistrationToken());
        }
    }

    private static String sep(ConfigFormat format) {
        return format instanceof JsonConfigFormat ? "\": " : "=";
    }

    @Test
    void identityClearedByAStaleEditorSaveIsRestored() throws Exception {
        Path path = dir.resolve("takaro.properties");
        ConfigFile cf = file(path, new PropertiesConfigFormat());
        String id = cf.load().getIdentityToken();

        // the editor still had the file from before the identity was written
        Files.writeString(path, new PropertiesConfigFormat().defaultText()
                .replace("registration_token=\n", "registration_token=tok\n"));
        TakaroConfig next = cf.poll();

        assertEquals(id, next.getIdentityToken(), "the identity of this run is kept");
        assertEquals(1, generated);
        assertTrue(Files.readString(path).contains("identity_token=" + id));
    }

    @Test
    void unreadableFileAtStartYieldsNoToken() throws Exception {
        Path path = dir.resolve("takaro.json");
        Files.writeString(path, "{ not json");
        TakaroConfig c = file(path, new JsonConfigFormat()).load();
        assertEquals("", c.getRegistrationToken());
        assertEquals(TakaroConfig.DEFAULT_WS_URL, c.getWsUrl());
    }

    @Test
    void anExistingIdentityBlankedByASaveIsPutBackNotReplaced() throws Exception {
        Path path = dir.resolve("takaro.properties");
        Files.writeString(path, "takaro.authentication.identity_token=my-smp\n"
                + "takaro.authentication.registration_token=tok\n");
        ConfigFile cf = file(path, new PropertiesConfigFormat());
        assertEquals("my-smp", cf.load().getIdentityToken());

        Files.writeString(path, "takaro.authentication.identity_token=\n"
                + "takaro.authentication.registration_token=tok\n");
        TakaroConfig next = cf.poll();

        assertEquals("my-smp", next.getIdentityToken());
        assertEquals(0, generated, "an install that has an identity never gets a new one");
        assertTrue(Files.readString(path).contains("identity_token=my-smp"));
    }

    @Test
    void aSaveStillBeingWrittenIsNotApplied() throws Exception {
        Path path = dir.resolve("takaro.properties");
        Files.writeString(path, "takaro.authentication.identity_token=id\n"
                + "takaro.authentication.registration_token=\n");
        ConfigFile cf = new ConfigFile(path, new PropertiesConfigFormat(), log, env::get, ids, 300);
        cf.load();

        Files.writeString(path, "takaro.authentication.identity_token=id\n"
                + "takaro.authentication.registration_token=par\n");
        Thread writer = new Thread(() -> {
            try {
                Thread.sleep(100);
                Files.writeString(path, "takaro.authentication.identity_token=id\n"
                        + "takaro.authentication.registration_token=partial-then-complete\n");
            } catch (Exception ignored) {
            }
        });
        writer.start();
        assertNull(cf.poll(), "the half-written token must not be used");
        writer.join();
        assertEquals("partial-then-complete", cf.poll().getRegistrationToken());
    }
}
