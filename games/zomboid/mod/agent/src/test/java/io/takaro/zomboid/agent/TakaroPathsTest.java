package io.takaro.zomboid.agent;

import org.junit.jupiter.api.Test;

import java.nio.file.Path;
import java.nio.file.Paths;

import static org.junit.jupiter.api.Assertions.*;

class TakaroPathsTest {

    private static final Path JAR = Paths.get("/srv/pz/Zomboid/Takaro");
    private static final String HOME = "/home/someone";

    @Test
    void theConfigLivesInTheJarFolder() {
        assertEquals(JAR, TakaroPaths.resolveDataDir(null, JAR, HOME));
    }

    @Test
    void noJarFolderFallsBackToTheHomeZomboidFolder() {
        assertEquals(Paths.get(HOME, "Zomboid", "Takaro"), TakaroPaths.resolveDataDir(null, null, HOME));
    }

    @Test
    void configOverrideDecidesTheFolder() {
        assertEquals(Paths.get("/opt/takaro").toAbsolutePath(),
                TakaroPaths.resolveDataDir("/opt/takaro/my.txt", JAR, HOME));
    }
}
