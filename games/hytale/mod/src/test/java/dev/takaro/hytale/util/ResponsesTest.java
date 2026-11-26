package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class ResponsesTest {

    @Test
    void rawResultIsAlwaysAString() {
        Map<String, Object> ok = Responses.commandResult(true, "hello");
        assertEquals(true, ok.get("success"));
        assertInstanceOf(String.class, ok.get("rawResult"));
        assertEquals("hello", ok.get("rawResult"));
    }

    @Test
    void nullRawResultBecomesEmptyStringNotNull() {
        Map<String, Object> r = Responses.commandResult(false, null);
        assertInstanceOf(String.class, r.get("rawResult"));
        assertEquals("", r.get("rawResult"));
        assertEquals(false, r.get("success"));
    }

    @Test
    void commandNameStripsArgumentsAndLeadingSlash() {
        assertEquals("who", Responses.commandName("who"));
        assertEquals("who", Responses.commandName("/who"));
        assertEquals("give", Responses.commandName("give Mad001 Wood_Oak_Trunk 10"));
        assertEquals("kick", Responses.commandName("  /kick Someone rude  "));
        assertEquals("", Responses.commandName(""));
        assertEquals("", Responses.commandName(null));
    }

    @Test
    void ansiEscapesAreStripped() {
        assertEquals("Hello", Responses.stripAnsi("\u001B[31mHello\u001B[0m"));
        assertEquals("plain", Responses.stripAnsi("plain"));
        assertEquals("", Responses.stripAnsi(null));
    }
}
