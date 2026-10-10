package io.takaro.zomboid.agent;

import org.junit.jupiter.api.Test;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Set;

import static org.junit.jupiter.api.Assertions.*;

class TakaroPathsTest {

    private static final Path JAR = Paths.get("/srv/pz/Zomboid/Takaro");
    private static final String HOME = "/home/someone";
    private static final Path HOME_DIR = Paths.get(HOME, "Zomboid", "Takaro");
    private static final Path LEGACY = Paths.get(TakaroPaths.LEGACY_DIR);

    private static Path resolve(Path jarDir, Set<Path> existing) {
        return TakaroPaths.resolveDataDir(null, jarDir, HOME, existing::contains);
    }

    @Test
    void freshInstallUsesTheJarFolder() {
        assertEquals(JAR, resolve(JAR, Set.of()));
    }

    @Test
    void noJarFolderFallsBackToTheHomeZomboidFolder() {
        assertEquals(HOME_DIR, resolve(null, Set.of()));
    }

    @Test
    void anExistingLegacyInstallKeepsItsFolderWhereverTheJarIs() {
        assertEquals(LEGACY, resolve(JAR, Set.of(LEGACY.resolve("TakaroConfig.txt"))));
        assertEquals(LEGACY, resolve(JAR, Set.of(LEGACY.resolve("bans.json"))));
    }

    @Test
    void theJarFolderWinsWhenItAlreadyHoldsAConfig() {
        assertEquals(JAR, resolve(JAR, Set.of(JAR.resolve("TakaroConfig.txt"), LEGACY.resolve("TakaroConfig.txt"))));
    }

    @Test
    void windowsStyleHomeFolderIsFoundByItsFiles() {
        assertEquals(HOME_DIR, resolve(JAR, Set.of(HOME_DIR.resolve("TakaroConfig.saved.txt"))));
    }

    @Test
    void configOverrideDecidesTheFolder() {
        assertEquals(Paths.get("/opt/takaro").toAbsolutePath(),
                TakaroPaths.resolveDataDir("/opt/takaro/my.txt", JAR, HOME, p -> true));
    }
}
