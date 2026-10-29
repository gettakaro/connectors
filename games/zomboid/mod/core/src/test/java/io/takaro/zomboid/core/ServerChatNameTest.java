package io.takaro.zomboid.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class ServerChatNameTest {

    @Test
    void overrideWinsOverConfig() {
        assertEquals("Takaro", ServerChatName.resolve("Takaro", "Config Name"));
    }

    @Test
    void configIsTheFallback() {
        assertEquals("Config Name", ServerChatName.resolve(null, "Config Name"));
    }

    @Test
    void namesAreTrimmed() {
        assertEquals("Takaro", ServerChatName.resolve("  Takaro\t", null));
        assertEquals("Config Name", ServerChatName.resolve(null, " Config Name "));
    }

    @Test
    void emptyOrWhitespaceCountsAsUnset() {
        assertEquals("Config Name", ServerChatName.resolve("", "Config Name"));
        assertEquals("Config Name", ServerChatName.resolve("   ", "Config Name"));
        assertNull(ServerChatName.resolve(" ", "\t"));
        assertNull(ServerChatName.resolve(null, null));
    }

    @Test
    void renderPutsTheSenderInFrontOfTheText() {
        assertEquals("Takaro: hello", ServerChatName.render("Takaro", "Dev Server", "hello"));
    }

    @Test
    void renderWithoutANameKeepsTheServerNamePrefix() {
        assertEquals("Dev Server: hello", ServerChatName.render(null, "Dev Server", "hello"));
    }

    @Test
    void renderWithoutAnyNameSaysServer() {
        assertEquals("Server: hello", ServerChatName.render(null, null, "hello"));
        assertEquals("Server: hello", ServerChatName.render(null, "", "hello"));
    }
}
