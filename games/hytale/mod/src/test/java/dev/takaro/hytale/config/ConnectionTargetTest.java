package dev.takaro.hytale.config;

import dev.takaro.hytale.websocket.TakaroWebSocket;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class ConnectionTargetTest {

    private static ConnectionTarget resolve(Map<String, String> values) {
        return ConnectionTarget.resolve(values::get);
    }

    @Test
    void publicTakaroOnlyWhenNothingIsConfigured() {
        Map<String, String> v = new HashMap<>();
        v.put("IDENTITY_TOKEN", "my-server");
        v.put("REGISTRATION_TOKEN", "reg");
        ConnectionTarget t = resolve(v);
        assertEquals(ConnectionTarget.DEFAULT_URL, t.url());
        assertEquals("my-server", t.identityToken());
        assertEquals("reg", t.registrationToken());
        assertFalse(t.dev());
    }

    @Test
    void configuredUrlReplacesThePublicOne() {
        Map<String, String> v = new HashMap<>();
        v.put("TAKARO_WS_URL", " wss://takaro.example/ ");
        v.put("IDENTITY_TOKEN", "my-server");
        v.put("DEV_ENABLED", "true");
        v.put("DEV_WS_URL", "wss://dev.example/");
        ConnectionTarget t = resolve(v);
        assertEquals("wss://takaro.example/", t.url());
        assertEquals("my-server", t.identityToken());
        assertFalse(t.dev());
    }

    @Test
    void devSettingsAreTheOnlyConnectionWhenEnabled() {
        Map<String, String> v = new HashMap<>();
        v.put("IDENTITY_TOKEN", "public-name");
        v.put("REGISTRATION_TOKEN", "public-reg");
        v.put("DEV_ENABLED", "true");
        v.put("DEV_WS_URL", "wss://staging.example/");
        v.put("DEV_IDENTITY_TOKEN", "staging-name");
        v.put("DEV_REGISTRATION_TOKEN", "staging-reg");
        ConnectionTarget t = resolve(v);
        assertEquals("wss://staging.example/", t.url());
        assertEquals("staging-name", t.identityToken());
        assertEquals("staging-reg", t.registrationToken());
        assertTrue(t.dev());
    }

    @Test
    void blankUrlFallsBackToTheNextRule() {
        Map<String, String> v = new HashMap<>();
        v.put("TAKARO_WS_URL", "  ");
        v.put("DEV_ENABLED", "true");
        assertEquals(ConnectionTarget.DEFAULT_DEV_URL, resolve(v).url());
    }

    @Test
    void theSocketDialsOnlyTheConfiguredUrl(@TempDir Path dir) throws Exception {
        File file = dir.resolve("TakaroConfig.properties").toFile();
        Files.writeString(file.toPath(), String.join("\n",
            "IDENTITY_TOKEN=public-name",
            "REGISTRATION_TOKEN=public-reg",
            "DEV_ENABLED=true",
            "DEV_WS_URL=wss://staging.example/",
            "DEV_IDENTITY_TOKEN=staging-name",
            ""));
        TakaroConfig config = new TakaroConfig(file);
        TakaroWebSocket socket = new TakaroWebSocket(null, config);
        assertEquals("wss://staging.example/", socket.getURI().toString());
        assertEquals("wss://staging.example/", config.getWsUrl());
    }
}
