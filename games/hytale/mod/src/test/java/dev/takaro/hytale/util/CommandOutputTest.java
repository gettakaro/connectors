package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class CommandOutputTest {

    @Test
    void keepsSenderOutputThenThreadScopedLogOutput() {
        String merged = CommandOutput.merge(
            List.of("Commands (use \"/help <command>\" for details):"),
            List.of("ban    broadcast    echo", "gamemode    give    help"));

        assertEquals(
            "Commands (use \"/help <command>\" for details):\nban    broadcast    echo\ngamemode    give    help",
            merged);
    }

    @Test
    void dropsTheCommandManagerEcho() {
        assertTrue(CommandOutput.isExcluded("Takaro Console executed command: echo hi"));
        assertTrue(CommandOutput.isExcluded("Takaro Console sent command: echo hi"));
        assertTrue(CommandOutput.isExcluded("[wire] OUT type=response requestId=abc"));
        assertTrue(CommandOutput.isExcluded("   "));
        assertTrue(CommandOutput.isExcluded(null));
        assertFalse(CommandOutput.isExcluded("Players online: 1"));

        assertEquals("OWN-1", CommandOutput.merge(
            List.of("OWN-1"),
            List.of("Takaro Console executed command: echo OWN-1")));
    }

    @Test
    void neverMergesTheConnectorsOwnLoggers() {
        assertTrue(CommandOutput.isExcludedLogger("takaro.wire"));
        assertTrue(CommandOutput.isExcludedLogger("dev.takaro.hytale.handlers.TakaroRequestHandler"));
        assertFalse(CommandOutput.isExcludedLogger("com.hypixel.hytale.server.core.command.system.CommandManager"));
        assertFalse(CommandOutput.isExcludedLogger(null));
    }

    @Test
    void deDuplicatesLinesPresentInBothHalves() {
        assertEquals("Players online: 1\nExamplePlayer",
            CommandOutput.merge(
                List.of("Players online: 1", "ExamplePlayer"),
                List.of("Players online: 1", "ExamplePlayer")));
    }

    @Test
    void splitsMultiLineMessagesAndStripsAnsi() {
        String merged = CommandOutput.merge(
            List.of("[32mheader[0m\nbody line 1\nbody line 2"), List.of());
        assertEquals("header\nbody line 1\nbody line 2", merged);
    }

    @Test
    void capsRunawayOutput() {
        List<String> many = new ArrayList<>();
        for (int i = 0; i < CommandOutput.MAX_LINES + 50; i++) {
            many.add("line-" + i);
        }
        String merged = CommandOutput.merge(many, List.of());
        String[] lines = merged.split("\n");
        assertEquals(CommandOutput.MAX_LINES + 1, lines.length);
        assertTrue(lines[lines.length - 1].contains("truncated"));
    }

    @Test
    void capsOnCharactersToo() {
        List<String> many = new ArrayList<>();
        String chunk = "x".repeat(500);
        for (int i = 0; i < 100; i++) {
            many.add(i + chunk);
        }
        String merged = CommandOutput.merge(many, List.of());
        assertTrue(merged.length() <= CommandOutput.MAX_CHARS + 80, "was " + merged.length());
        assertTrue(merged.endsWith("truncated by the Takaro connector]"));
    }

    @Test
    void tolerantOfNullHalves() {
        assertEquals("only", CommandOutput.merge(List.of("only"), null));
        assertEquals("only", CommandOutput.merge(null, List.of("only")));
        assertEquals("", CommandOutput.merge(null, null));
    }
}
