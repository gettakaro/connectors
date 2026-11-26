package dev.takaro.hytale.protocol;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class GameProfileTest {

    private static byte[] shippedIcon() throws Exception {
        String source = System.getProperty("takaro.iconSource");
        assertNotNull(source, "the build passes the icon path to the tests");
        return Files.readAllBytes(Path.of(source));
    }

    @Test
    void iconRevisionAndSizeMatchTheShippedFile() throws Exception {
        byte[] file = shippedIcon();
        GameProfile.Icon icon = GameProfile.icon();
        assertNotNull(icon, "icon-256.png is on the classpath");
        assertArrayEquals(file, icon.png());
        assertEquals(file.length, icon.size());
        String sha = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(file));
        assertEquals(sha.substring(0, 12), icon.revision());
    }

    @Test
    void iconIsA256SquarePngWithinTakarosLimit() throws Exception {
        byte[] png = GameProfile.icon().png();
        assertTrue(png.length <= 128 * 1024);
        byte[] signature = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        assertArrayEquals(signature, Arrays.copyOf(png, 8));
        ByteBuffer ihdr = ByteBuffer.wrap(png, 16, 8);
        assertEquals(256, ihdr.getInt());
        assertEquals(256, ihdr.getInt());
        assertEquals(256, javax.imageio.ImageIO.read(new ByteArrayInputStream(png)).getWidth());
    }

    @Test
    void connectorVersionIsTheBuildStampedOne() {
        String expected = System.getProperty("takaro.expectedVersion");
        assertNotNull(expected);
        assertEquals(expected, GameProfile.stampedVersion());
        assertTrue(GameProfile.isSemver(GameProfile.stampedVersion()));
        assertEquals(Map.of("name", "takaro-hytale", "version", expected), GameProfile.connector(expected));
    }

    @Test
    void anInvalidConnectorVersionIsLeftOut() {
        assertNull(GameProfile.connector("dev"));
        assertNull(GameProfile.connector(null));
        assertNull(GameProfile.connector("1.0"));
        assertNotNull(GameProfile.connector("0.1.0-beta.1+abc123"));
    }

    @Test
    void gameVersionIsTrimmedPrintableAsciiOfAtMost32() {
        assertEquals("2026.01.24-6e2d4fc36", GameProfile.gameVersion("  2026.01.24-6e2d4fc36 \n"));
        assertNull(GameProfile.gameVersion(null));
        assertNull(GameProfile.gameVersion("   "));
        assertNull(GameProfile.gameVersion("x".repeat(33)));
        assertEquals("x".repeat(32), GameProfile.gameVersion("x".repeat(32)));
        assertNull(GameProfile.gameVersion("1.0é"));
    }

    @Test
    void parametersCarryNameVersionAndIcon() {
        Map<String, Object> p = GameProfile.parameters("0.7.7");
        assertEquals("Hytale", p.get("name"));
        assertEquals("0.7.7", p.get("version"));
        GameProfile.Icon icon = GameProfile.icon();
        assertEquals(Map.of("revision", icon.revision(), "size", icon.size()), p.get("icon"));
        assertFalse(GameProfile.parameters(null).containsKey("version"));
    }
}
