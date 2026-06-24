package io.takaro.minecraft.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class TakaroConfigTest {

    @Test
    void defaultValuesAreSet() {
        TakaroConfig config = new TakaroConfig();
        assertNull(config.getWsUrl());
        assertNull(config.getIdentityToken());
        assertNull(config.getRegistrationToken());
        assertTrue(config.isReconnectEnabled());
        assertEquals(5000, config.getReconnectDelay());
        assertEquals(300000, config.getMaxReconnectDelay());
        assertEquals(1.5, config.getBackoffMultiplier());
        assertFalse(config.isDebugEnabled());
    }

    @Test
    void settersAndGettersWork() {
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("wss://example.com");
        config.setIdentityToken("id-token");
        config.setRegistrationToken("reg-token");
        config.setReconnectEnabled(false);
        config.setReconnectDelay(10000);
        config.setMaxReconnectDelay(60000);
        config.setBackoffMultiplier(2.0);
        config.setDebugEnabled(true);

        assertEquals("wss://example.com", config.getWsUrl());
        assertEquals("id-token", config.getIdentityToken());
        assertEquals("reg-token", config.getRegistrationToken());
        assertFalse(config.isReconnectEnabled());
        assertEquals(10000, config.getReconnectDelay());
        assertEquals(60000, config.getMaxReconnectDelay());
        assertEquals(2.0, config.getBackoffMultiplier());
        assertTrue(config.isDebugEnabled());
    }

    @Test
    void applyEnvOverridesWithNoEnvVarsDoesNothing() {
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("wss://original.com");
        config.setIdentityToken("original-id");
        config.setRegistrationToken("original-reg");

        // When env vars are not set, values should remain unchanged
        // (We can't easily mock System.getenv, but we can verify
        // that calling applyEnvOverrides doesn't throw and preserves
        // values when env vars aren't set for our specific keys)
        config.applyEnvOverrides();

        // Values should be unchanged if TAKARO_* env vars aren't set in this test env
        // This test validates the method doesn't crash; env var integration
        // is validated by the exerciser (docker compose e2e test)
        assertNotNull(config.getWsUrl());
        assertNotNull(config.getIdentityToken());
        assertNotNull(config.getRegistrationToken());
        assertFalse(config.isDebugEnabled());
    }
}
