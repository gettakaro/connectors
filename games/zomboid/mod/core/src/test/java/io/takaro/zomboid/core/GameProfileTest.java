package io.takaro.zomboid.core;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.*;

/** The shipped identity and icon against Takaro's game.profile and getGameIcon limits. */
class GameProfileTest {

    private static final byte[] PNG_SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    @Test
    void connectorVersionIsTheReleaseVersion() throws Exception {
        String versionTxt = Files.readString(Path.of(System.getProperty("takaro.versionFile")),
                StandardCharsets.UTF_8).trim();
        assertEquals(versionTxt, GameProfile.connectorVersion());
        assertTrue(GameProfile.connectorVersion().matches(
                "^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$"));
    }

    @Test
    void identifiersMatchTakarosSlugRule() {
        assertTrue(GameProfile.GAME.matches("^[a-z0-9][a-z0-9-]*$"));
        assertTrue(GameProfile.CONNECTOR_NAME.matches("^[a-z0-9][a-z0-9-]*$"));
        assertTrue(GameProfile.DISPLAY_NAME.length() <= 64);
    }

    @Test
    void iconIsTheCommittedAsset256SquarePngWithinTheLimit() throws Exception {
        byte[] icon = GameProfile.icon();
        assertArrayEquals(Files.readAllBytes(Path.of(System.getProperty("takaro.iconFile"))), icon);
        assertTrue(icon.length <= 131072, "icon is " + icon.length + " bytes");
        for (int i = 0; i < PNG_SIGNATURE.length; i++) assertEquals(PNG_SIGNATURE[i], icon[i]);
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(icon, 8, icon.length - 8));
        in.readInt();
        assertEquals("IHDR", new String(in.readNBytes(4), StandardCharsets.US_ASCII));
        assertEquals(256, in.readInt());
        assertEquals(256, in.readInt());
    }

    @Test
    void revisionIsTheFirst12HexOfTheSha256() {
        assertEquals(GameProfile.revisionOf(GameProfile.icon()), GameProfile.iconRevision());
        assertTrue(GameProfile.iconRevision().matches("^[0-9a-f]{12}$"));
    }

    @Test
    void gameVersionIsTrimmedToPrintableAsciiAnd32Chars() {
        assertEquals("42.20.4", GameProfile.sanitizeGameVersion("  42.20.4\n"));
        assertNull(GameProfile.sanitizeGameVersion("   "));
        assertNull(GameProfile.sanitizeGameVersion(null));
        assertEquals(32, GameProfile.sanitizeGameVersion("x".repeat(40)).length());
        assertEquals("42.20", GameProfile.sanitizeGameVersion("42.20é"));
    }
}
