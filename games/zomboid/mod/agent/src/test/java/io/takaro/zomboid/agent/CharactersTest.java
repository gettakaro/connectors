package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.GameCharacter;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.*;

class CharactersTest {

    @TempDir
    Path dir;

    private Characters characters() {
        return new Characters(() -> dir.resolve(Characters.FILE_NAME));
    }

    @Test
    void anUnseenAccountHasNoCharacterAndNeedsOne() {
        Characters characters = characters();
        assertNull(characters.current("bob"));
        assertTrue(characters.needsNew("bob", new Object()));
    }

    @Test
    void aLivingSurvivorKeepsItsIdAcrossARestart() {
        GameCharacter first = characters().begin("bob", "Valencia Gregory");
        Characters restarted = characters();
        assertEquals(first, restarted.current("bob"));
        assertFalse(restarted.needsNew("bob", new Object()));
    }

    @Test
    void theIdDoesNotComeFromTheName() {
        Characters characters = characters();
        assertNotEquals(characters.begin("bob", "Same Name").id(), characters.begin("amy", "Same Name").id());
    }

    @Test
    void aDeathReturnsTheSurvivorItEndedAndLeavesNoCurrentOne() {
        Characters characters = characters();
        GameCharacter first = characters.begin("bob", "One");
        assertEquals(first, characters.end("bob", new Object()));
        assertNull(characters.current("bob"));
    }

    @Test
    void theDeadBodyNeverGetsANewIdButTheNextSurvivorDoes() {
        Characters characters = characters();
        GameCharacter first = characters.begin("bob", "One");
        Object body = new Object();
        characters.end("bob", body);
        assertFalse(characters.needsNew("bob", body));
        assertTrue(characters.needsNew("bob", new Object()));
        assertNotEquals(first.id(), characters.begin("bob", "Two").id());
    }

    @Test
    void aDeathSurvivesARestartSoTheNextLoginIsANewSurvivor() {
        Characters characters = characters();
        characters.begin("bob", "One");
        characters.end("bob", new Object());
        Characters restarted = characters();
        assertNull(restarted.current("bob"));
        assertTrue(restarted.needsNew("bob", new Object()));
    }

    @Test
    void endingAnUnknownAccountEndsNothing() {
        assertNull(characters().end("bob", new Object()));
    }

    @Test
    void anEntryWithoutAnIdIsIgnoredSoTheSurvivorGetsOne() throws Exception {
        Files.writeString(dir.resolve(Characters.FILE_NAME), "{\"bob\":{}}");
        Characters characters = characters();
        assertNull(characters.current("bob"));
        assertTrue(characters.needsNew("bob", new Object()));
    }

    @Test
    void anUnreadableFileStartsEmpty() throws Exception {
        Files.writeString(dir.resolve(Characters.FILE_NAME), "not json");
        assertNull(characters().current("bob"));
    }
}
