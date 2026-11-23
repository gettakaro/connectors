package io.takaro.minecraft.core;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.junit.jupiter.api.Test;

class ConsoleChatTest {

    @Test
    void readsTheTextOfEverySpellingOfSay() {
        assertEquals("hello there", ConsoleChat.sayText("say hello there"));
        assertEquals("hello", ConsoleChat.sayText("/say hello"));
        assertEquals("hello", ConsoleChat.sayText("minecraft:say hello"));
        assertEquals("hello", ConsoleChat.sayText("/minecraft:say   hello "));
    }

    @Test
    void leavesEverythingElseAlone() {
        assertNull(ConsoleChat.sayText(null));
        assertNull(ConsoleChat.sayText("say"));
        assertNull(ConsoleChat.sayText("say   "));
        assertNull(ConsoleChat.sayText("saying hello"));
        assertNull(ConsoleChat.sayText("list"));
    }
}
