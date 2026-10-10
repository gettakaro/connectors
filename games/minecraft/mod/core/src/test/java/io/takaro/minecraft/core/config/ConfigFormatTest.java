package io.takaro.minecraft.core.config;

import io.takaro.minecraft.core.TakaroConfig;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class ConfigFormatTest {

    @Test
    void jsonFillsTheIdentityInPlace() throws Exception {
        JsonConfigFormat f = new JsonConfigFormat();
        String out = f.withIdentity(f.defaultText(), "abc");
        assertTrue(out.contains("\"identity_token\": \"abc\""));
        assertEquals(f.defaultText().replace("\"identity_token\": \"\"", "\"identity_token\": \"abc\""), out);
    }

    @Test
    void jsonAddsAMissingIdentity() throws Exception {
        JsonConfigFormat f = new JsonConfigFormat();
        String out = f.withIdentity("{\"authentication\":{\"registration_token\":\"t\"}}", "abc");
        TakaroConfig c = f.parse(out);
        assertEquals("abc", c.getIdentityToken());
        assertEquals("t", c.getRegistrationToken());
    }

    @Test
    void jsonPre04FileStillParses() throws Exception {
        // what 0.3 wrote
        String old = "{\n  \"websocket\": {\n    \"url\": \"\"\n  },\n  \"authentication\": {\n"
                + "    \"identity_token\": \"x\",\n    \"registration_token\": \"t\"\n  },\n"
                + "  \"reconnect\": {\n    \"enabled\": true,\n    \"delay\": 5000,\n    \"max_delay\": 300000,\n"
                + "    \"backoff_multiplier\": 1.5\n  },\n  \"settings\": {\n    \"debug\": true\n  }\n}";
        TakaroConfig c = new JsonConfigFormat().parse(old);
        assertEquals("", c.getWsUrl());
        assertEquals("x", c.getIdentityToken());
        assertTrue(c.isDebugEnabled());
    }

    @Test
    void propertiesFillsTheIdentityAndKeepsComments() throws Exception {
        PropertiesConfigFormat f = new PropertiesConfigFormat();
        String out = f.withIdentity(f.defaultText(), "abc");
        assertTrue(out.contains("\ntakaro.authentication.identity_token=abc\n"));
        assertTrue(out.startsWith("# Takaro connector settings."));
        assertEquals("abc", f.parse(out).getIdentityToken());
    }

    @Test
    void propertiesAppendsAMissingIdentity() throws Exception {
        PropertiesConfigFormat f = new PropertiesConfigFormat();
        String out = f.withIdentity("takaro.authentication.registration_token=t", "abc");
        assertEquals("abc", f.parse(out).getIdentityToken());
        assertEquals("t", f.parse(out).getRegistrationToken());
    }

    @Test
    void propertiesRejectsABadNumber() {
        assertThrows(NumberFormatException.class, () -> new PropertiesConfigFormat()
                .parse("takaro.authentication.registration_token=t\ntakaro.reconnect.delay=5o00\n"));
    }

    @Test
    void yamlFillsTheIdentityAndKeepsComments() {
        String yaml = "takaro:\n  authentication:\n    # keep me\n    identity_token: \"\"  # note\n"
                + "    registration_token: \"\"\n";
        String out = YamlText.withIdentity(yaml, "abc");
        assertEquals("takaro:\n  authentication:\n    # keep me\n    identity_token: \"abc\" # note\n"
                + "    registration_token: \"\"\n", out);
        assertEquals("    identity_token: \"abc\"\n",
                YamlText.withIdentity("    identity_token:\n", "abc"));
        assertNull(YamlText.withIdentity("    identity_token: \"set\"\n", "abc"), "a set identity is left alone");
    }
}
