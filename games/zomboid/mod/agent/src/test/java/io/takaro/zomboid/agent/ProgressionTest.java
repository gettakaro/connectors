package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.ProgressionTrack;
import org.junit.jupiter.api.Test;

import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class ProgressionTest {

    @Test
    void anUnknownPlayerHasNoReading() {
        assertNull(new Progression().get("bob"));
    }

    @Test
    void aKnownPlayerKeepsTheLatestReading() {
        Progression progression = new Progression();
        progression.put("bob", List.of(new ProgressionTrack("axe", "Axe", 1, 80.0, 0.05)));
        progression.put("bob", List.of(new ProgressionTrack("axe", "Axe", 2, 180.0, 0.0)));
        assertEquals(2, progression.get("bob").get(0).level());
    }

    @Test
    void anEmptyReadingStaysDistinctFromUnknown() {
        Progression progression = new Progression();
        progression.put("bob", List.of());
        assertEquals(List.of(), progression.get("bob"));
    }
}
