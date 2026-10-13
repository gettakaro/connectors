package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.ProtocolException;
import io.takaro.zomboid.core.model.MapDimension;
import io.takaro.zomboid.core.model.MapImageChunk;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

import static org.junit.jupiter.api.Assertions.*;

/** The world-map raster from pyramid.zip, built from a small synthetic pyramid. */
class PyramidMapTest {

    @TempDir
    Path tmp;

    @Test
    void parsesBoundsAndImageSize() {
        PyramidMap.Info info = PyramidMap.parseInfo("VERSION=1\nbounds=0 0 19968 16128\nimageSize=19968 16128\n");
        assertEquals(new PyramidMap.Info(0, 0, 19968, 16128, 19968, 16128), info);
        assertThrows(IllegalArgumentException.class, () -> PyramidMap.parseInfo("VERSION=1\n"));
        assertThrows(IllegalArgumentException.class, () -> PyramidMap.parseInfo("bounds=5 0 5 10\nimageSize=1 1"));
    }

    @Test
    void choosesTheMostDetailedLevelThatFitsTakarosLimit() {
        PyramidMap.Info knox = new PyramidMap.Info(0, 0, 19968, 16128, 19968, 16128);
        // Level 1 is 9984 px wide (> 8192); level 2 is 4992 x 4032.
        assertEquals(2, PyramidMap.chooseLevel(knox, Set.of(0, 1, 2, 3, 4), PyramidMap.MAX_IMAGE_PX));
        assertEquals(3, PyramidMap.chooseLevel(knox, Set.of(0, 1, 3), PyramidMap.MAX_IMAGE_PX));
        assertEquals(-1, PyramidMap.chooseLevel(knox, Set.of(0, 1), PyramidMap.MAX_IMAGE_PX));
        assertEquals(4992, PyramidMap.levelSize(19968, 2));
        assertEquals(4032, PyramidMap.levelSize(16128, 2));
    }

    @Test
    void joinsTilesAtTheirGridPositionOverTheBackground() throws Exception {
        // 600 x 300 squares; at level 1 that is 300 x 150 px: two 256 px tiles across.
        Path zip = pyramid(tmp.resolve("pyramid.zip"), 600, 300, 1, 0xFFFF0000, 0x00000000);
        try (ZipFile z = new ZipFile(zip.toFile())) {
            BufferedImage image = PyramidMap.compose(z, PyramidMap.parseInfo(info(600, 300)), 1);
            assertEquals(300, image.getWidth());
            assertEquals(150, image.getHeight());
            assertEquals(0xFF0000, image.getRGB(10, 10) & 0xFFFFFF, "tile 0x0 is opaque red");
            assertEquals(PyramidMap.BACKGROUND_RGB, image.getRGB(280, 10) & 0xFFFFFF, "tile 1x0 is transparent");
        }
    }

    @Test
    void renderEncodesAJpegOfTheChosenLevel() throws Exception {
        Path zip = pyramid(tmp.resolve("pyramid.zip"), 600, 300, 0, 0xFF00FF00, 0xFF0000FF);
        try (ZipFile z = new ZipFile(zip.toFile())) {
            PyramidMap.Rendered r = PyramidMap.render(z, 0.9f);
            assertEquals(0, r.level());
            assertEquals(600, r.width());
            BufferedImage decoded = ImageIO.read(new ByteArrayInputStream(r.jpeg()));
            assertEquals(600, decoded.getWidth());
            assertEquals(300, decoded.getHeight());
            assertEquals(0xFF, r.jpeg()[0] & 0xFF);
            assertEquals(0xD8, r.jpeg()[1] & 0xFF);
        }
    }

    @Test
    void alphaBlendsOverTheBackground() {
        assertEquals(0x123456, PyramidMap.over(0xFF123456, 0x000000));
        assertEquals(0xABCDEF, PyramidMap.over(0x00123456, 0xABCDEF));
        int half = PyramidMap.over(0x80FFFFFF, 0x000000);
        assertEquals(0x80, (half >> 16) & 0xFF);
    }

    @Test
    void mapServiceDeclaresAKnoxCountyDimensionWithYDownAndCachesTheImage() throws Exception {
        Path mapDir = Files.createDirectories(tmp.resolve("media/maps/Muldraugh, KY"));
        Path zip = pyramid(mapDir.resolve("pyramid.zip"), 600, 300, 0, 0xFF00FF00, 0xFF0000FF);
        Path cache = tmp.resolve("cache");

        MapService.Loaded first = MapService.load(zip, cache);
        MapDimension d = first.dimension();
        assertEquals("knox-county", d.id());
        assertEquals("Knox County", d.name());
        assertEquals("x", d.horizontalField());
        assertEquals("right", d.horizontalDirection());
        assertEquals("y", d.verticalField());
        assertEquals("down", d.verticalDirection());
        assertEquals(600, d.horizontalMax());
        assertEquals(300, d.verticalMax());
        assertEquals(first.image().length, d.image().size());
        assertEquals("image/jpeg", d.image().mimeType());
        assertTrue(d.image().revision().matches("^pz-[0-9a-f]{16}-q88$"), d.image().revision());
        assertTrue(Files.isRegularFile(cache.resolve(d.image().revision() + ".jpg")));

        MapService.Loaded second = MapService.load(zip, cache);
        assertEquals(d, second.dimension(), "the cached copy declares the same image");
        assertArrayEquals(first.image(), second.image());
    }

    @Test
    void chunksFollowTheProtocol() throws Exception {
        Path zip = pyramid(Files.createDirectories(tmp.resolve("m/Muldraugh, KY")).resolve("pyramid.zip"),
                600, 300, 0, 0xFF00FF00, 0xFF0000FF);
        MapService.Loaded loaded = MapService.load(zip, tmp.resolve("c"));
        int size = loaded.image().length;
        MapImageChunk last = MapService.chunk(loaded, size - 3, 1024);
        assertEquals(3, last.data().length, "the last chunk is shorter");
        assertEquals(loaded.dimension().image().revision(), last.revision());
        ProtocolException past = assertThrows(ProtocolException.class, () -> MapService.chunk(loaded, size, 1));
        assertEquals("invalid_args", past.code());

        MapService empty = new MapService();
        assertTrue(empty.dimensions().isEmpty(), "nothing declared until the image is ready");
        assertEquals("not_found", assertThrows(ProtocolException.class, () -> empty.read("knox-county", 0, 1)).code());
    }

    @Test
    void findsThePyramidOfTheConfiguredMapAndFallsBackToTheVanillaMap() throws Exception {
        Path install = tmp.resolve("install");
        Path vanilla = Files.createDirectories(install.resolve("media/maps/Muldraugh, KY"));
        Files.writeString(vanilla.resolve("pyramid.zip"), "x");
        assertEquals(vanilla.resolve("pyramid.zip"), MapService.findPyramid(install, "SomeModMap;Muldraugh, KY"));
        assertEquals(vanilla.resolve("pyramid.zip"), MapService.findPyramid(install, null));
        Path mod = Files.createDirectories(install.resolve("media/maps/Raven Creek"));
        Files.writeString(mod.resolve("pyramid.zip"), "x");
        assertEquals(mod.resolve("pyramid.zip"), MapService.findPyramid(install, "Raven Creek;Muldraugh, KY"));
        assertNull(MapService.findPyramid(tmp.resolve("nothing"), "Muldraugh, KY"));
        assertArrayEquals(new String[] {"raven-creek", "Raven Creek"}, MapService.dimensionIdName("Raven Creek"));
    }

    // --- synthetic pyramid: level 0 (and level 1) with two tiles across, filled with two colours ---

    private static String info(int w, int h) {
        return "VERSION=1\nbounds=0 0 " + w + " " + h + "\nimageSize=" + w + " " + h + "\n";
    }

    private static Path pyramid(Path out, int w, int h, int level, int leftArgb, int rightArgb) throws IOException {
        Files.createDirectories(out.getParent());
        try (OutputStream file = Files.newOutputStream(out); ZipOutputStream zip = new ZipOutputStream(file)) {
            zip.putNextEntry(new ZipEntry("pyramid.txt"));
            zip.write(info(w, h).getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            int lw = PyramidMap.levelSize(w, level);
            int lh = PyramidMap.levelSize(h, level);
            for (int ty = 0; ty * 256 < lh; ty++) {
                for (int tx = 0; tx * 256 < lw; tx++) {
                    BufferedImage tile = new BufferedImage(256, 256, BufferedImage.TYPE_INT_ARGB);
                    int argb = tx == 0 ? leftArgb : rightArgb;
                    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) tile.setRGB(x, y, argb);
                    ByteArrayOutputStream png = new ByteArrayOutputStream();
                    ImageIO.write(tile, "png", png);
                    zip.putNextEntry(new ZipEntry(level + "/tile" + tx + "x" + ty + ".png"));
                    zip.write(png.toByteArray());
                    zip.closeEntry();
                }
            }
        }
        return out;
    }
}
