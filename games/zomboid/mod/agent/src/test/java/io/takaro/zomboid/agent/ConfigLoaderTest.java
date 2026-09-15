package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.TakaroConfig;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.*;

class ConfigLoaderTest {

    @AfterEach
    void clearProp() {
        System.clearProperty("takaro.configFile");
    }

    @Test
    void parsesKeyValueWithComments(@TempDir Path dir) throws Exception {
        Path cfg = dir.resolve("TakaroConfig.txt");
        Files.writeString(cfg, String.join("\n",
                "# Takaro config",
                "wsUrl=wss://connect.example/   # trailing comment",
                "identityToken=my-identity",
                "registrationToken=my-registration",
                "debug=true",
                "logEvents=yes",
                "",
                "unknownKey=ignored"));
        System.setProperty("takaro.configFile", cfg.toString());

        ConfigLoader loader = new ConfigLoader();
        TakaroConfig config = loader.load();

        // No TAKARO_* env vars in the test env, so file values survive overrides.
        assertEquals("wss://connect.example/", config.getWsUrl());
        assertEquals("my-identity", config.getIdentityToken());
        assertEquals("my-registration", config.getRegistrationToken());
        assertTrue(config.isDebugEnabled());
        assertTrue(loader.isLogEvents());
    }

    @Test
    void missingFileDoesNotThrow(@TempDir Path dir) {
        System.setProperty("takaro.configFile", dir.resolve("absent.txt").toString());
        ConfigLoader loader = new ConfigLoader();
        TakaroConfig config = assertDoesNotThrow(loader::load);
        assertNull(config.getWsUrl());
        assertFalse(loader.isLogEvents());
    }
}
