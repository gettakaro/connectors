package io.takaro.minecraft.paper;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import java.util.Map;
import java.util.function.Function;

import static org.junit.jupiter.api.Assertions.assertEquals;

/**
 * The name resolution a Takaro admin sees, without a server: the lookup is injected, so
 * neither branch needs NMS on the classpath.
 */
class PaperNamesTest {

    @Test
    @DisplayName("a key path becomes a human name when no language table is available")
    void titleCasesTheKeyPath() {
        assertEquals("Diamond Sword", PaperNames.titleCase("diamond_sword"));
        assertEquals("Tnt", PaperNames.titleCase("tnt"));
        assertEquals("Zombie Villager", PaperNames.titleCase("zombie_villager"));
        assertEquals("Diamond Sword", PaperNames.titleCase("minecraft:diamond_sword"));
        assertEquals("", PaperNames.titleCase(null));
    }

    @Test
    @DisplayName("the language table wins over the fallback when it knows the key")
    void prefersTheTranslatedName() {
        Function<String, String> table = Map.of("item.minecraft.tnt", "TNT")::get;
        assertEquals("TNT", PaperNames.displayName("item.minecraft.tnt", "tnt", table));
    }

    @Test
    @DisplayName("a language table that echoes the key back is not a name")
    void ignoresAnEchoedKey() {
        Function<String, String> echo = key -> key;
        assertEquals("Diamond Sword", PaperNames.displayName("item.minecraft.diamond_sword", "diamond_sword", echo));
    }

    @Test
    @DisplayName("no table, an empty answer or no key all fall back to the key path")
    void fallsBackOnEveryMissingPiece() {
        assertEquals("Zombie", PaperNames.displayName("entity.minecraft.zombie", "zombie", null));
        assertEquals("Zombie", PaperNames.displayName("entity.minecraft.zombie", "zombie", key -> ""));
        assertEquals("Zombie", PaperNames.displayName(null, "zombie", key -> "unused"));
    }
}
