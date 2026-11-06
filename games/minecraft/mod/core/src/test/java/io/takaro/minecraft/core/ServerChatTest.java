package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.ChatLine;
import org.junit.jupiter.api.Test;

import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class ServerChatTest {

    @Test
    void theOverrideWinsOverTheConfiguredName() {
        assertEquals("Herald", ServerChat.resolveSender("Herald", "Config"));
    }

    @Test
    void theConfiguredNameIsTheFallback() {
        assertEquals("Config", ServerChat.resolveSender(null, "Config"));
        assertEquals("Config", ServerChat.resolveSender("", "Config"));
        assertEquals("Config", ServerChat.resolveSender(" \t ", "Config"));
    }

    @Test
    void blankEverywhereMeansNoSender() {
        assertNull(ServerChat.resolveSender(null, null));
        assertNull(ServerChat.resolveSender("", ""));
        assertNull(ServerChat.resolveSender("  ", "  "));
    }

    @Test
    void namesAreTrimmed() {
        assertEquals("Herald", ServerChat.resolveSender("  Herald ", null));
        assertEquals("My Server", ServerChat.resolveSender(null, " My Server "));
    }

    @Test
    void globalLineUsesTheSayFormat() {
        ChatLine line = ServerChat.line("hello all", null, "Herald", null);
        assertEquals(ChatLine.GLOBAL_KEY, line.translationKey());
        assertEquals("[Herald] hello all", line.plainText());
        assertFalse(line.whisper());
    }

    @Test
    void directedLineUsesTheWhisperFormat() {
        ChatLine line = ServerChat.line("just you", "uuid-1", null, "Herald");
        assertEquals(ChatLine.DIRECTED_KEY, line.translationKey());
        assertEquals("Herald whispers to you: just you", line.plainText());
        assertTrue(line.whisper());
    }

    @Test
    void withoutASenderTheTextIsUnchanged() {
        ChatLine global = ServerChat.line("[D] Ann: hi", null, " ", "");
        ChatLine directed = ServerChat.line("psst", "uuid-1", null, null);
        assertEquals("[D] Ann: hi", global.plainText());
        assertEquals("psst", directed.plainText());
        assertFalse(global.hasSender());
        assertFalse(directed.whisper());
    }

    @Test
    void theEnvironmentVariableSetsTheName() {
        TakaroConfig config = new TakaroConfig();
        config.setServerChatName("File");
        config.applyEnvOverrides(Map.of("TAKARO_SERVER_CHAT_NAME", "Env")::get);
        assertEquals("Env", config.getServerChatName());
        config.applyEnvOverrides(Map.of("TAKARO_SERVER_CHAT_NAME", "")::get);
        assertEquals("Env", config.getServerChatName(), "an empty variable leaves the file value");
    }

    @Test
    void aNameChangeNeedsNoReconnect() {
        TakaroConfig a = new TakaroConfig();
        TakaroConfig b = new TakaroConfig();
        b.setServerChatName("New");
        assertTrue(a.sameConnection(b));
        assertFalse(a.sameSettings(b));
        a.copyFrom(b);
        assertEquals("New", a.getServerChatName());
    }
}
