package io.takaro.zomboid.agent;

import javax.imageio.IIOImage;
import javax.imageio.ImageIO;
import javax.imageio.ImageWriteParam;
import javax.imageio.ImageWriter;
import javax.imageio.stream.ImageOutputStream;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.Iterator;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Reads the world-map raster that every Project Zomboid install ships beside its map data,
 * {@code media/maps/<map>/pyramid.zip}: the image the in-game world map (M) draws. It holds
 * {@code pyramid.txt} ({@code bounds=minX minY maxX maxY} in world squares and
 * {@code imageSize=w h}, one pixel per square at level 0) and 256 px tiles
 * {@code <level>/tile<x>x<y>.png}, each level half the size of the one before.
 *
 * <p>{@link #render} joins one level into a single JPEG for Takaro's {@code image} map source.
 * Pure Java2D raster code with no {@code Graphics}, so it needs no display or AWT toolkit.
 */
public final class PyramidMap {

    /** Takaro's limit for an {@code image} map source, per side. */
    public static final int MAX_IMAGE_PX = 8192;
    /** The in-game map's colour for squares with no terrain, used under transparent pixels. */
    public static final int BACKGROUND_RGB = 0xA0A0A4;

    private static final Pattern TILE = Pattern.compile("^(\\d+)/tile(\\d+)x(\\d+)\\.png$");

    /** What {@code pyramid.txt} says: world bounds and the level-0 image size. */
    public record Info(int minX, int minY, int maxX, int maxY, int imageWidth, int imageHeight) {
    }

    /** One level joined into an encoded image covering exactly {@link Info}'s bounds. */
    public record Rendered(Info info, int level, int width, int height, byte[] jpeg) {
    }

    private PyramidMap() {
    }

    static Info parseInfo(String text) {
        int[] bounds = null;
        int[] size = null;
        for (String line : text.split("\\R")) {
            String trimmed = line.trim();
            if (trimmed.startsWith("bounds=")) {
                bounds = ints(trimmed.substring("bounds=".length()), 4);
            } else if (trimmed.startsWith("imageSize=")) {
                size = ints(trimmed.substring("imageSize=".length()), 2);
            }
        }
        if (bounds == null || size == null) {
            throw new IllegalArgumentException("pyramid.txt has no bounds or imageSize");
        }
        if (bounds[2] <= bounds[0] || bounds[3] <= bounds[1] || size[0] <= 0 || size[1] <= 0) {
            throw new IllegalArgumentException("pyramid.txt has empty bounds or imageSize");
        }
        return new Info(bounds[0], bounds[1], bounds[2], bounds[3], size[0], size[1]);
    }

    /** The most detailed level whose joined image fits {@code maxPx} on both sides, or -1. */
    static int chooseLevel(Info info, Set<Integer> levels, int maxPx) {
        for (int level : new TreeSet<>(levels)) {
            if (levelSize(info.imageWidth(), level) <= maxPx && levelSize(info.imageHeight(), level) <= maxPx) {
                return level;
            }
        }
        return -1;
    }

    static int levelSize(int level0Size, int level) {
        return (int) Math.ceil(level0Size / (double) (1L << level));
    }

    public static Rendered render(ZipFile zip, float jpegQuality) throws IOException {
        ZipEntry txt = zip.getEntry("pyramid.txt");
        if (txt == null) {
            throw new IOException("pyramid.zip has no pyramid.txt");
        }
        Info info;
        try (InputStream in = zip.getInputStream(txt)) {
            info = parseInfo(new String(in.readAllBytes(), StandardCharsets.UTF_8));
        }
        Set<Integer> levels = new TreeSet<>();
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            Matcher m = TILE.matcher(e.nextElement().getName());
            if (m.matches()) {
                levels.add(Integer.parseInt(m.group(1)));
            }
        }
        int level = chooseLevel(info, levels, MAX_IMAGE_PX);
        if (level < 0) {
            throw new IOException("pyramid.zip has no level that fits " + MAX_IMAGE_PX + " px");
        }
        BufferedImage image = compose(zip, info, level);
        return new Rendered(info, level, image.getWidth(), image.getHeight(), encodeJpeg(image, jpegQuality));
    }

    /** Joins every tile of one level onto the background colour (alpha blended). */
    static BufferedImage compose(ZipFile zip, Info info, int level) throws IOException {
        int width = levelSize(info.imageWidth(), level);
        int height = levelSize(info.imageHeight(), level);
        BufferedImage out = new BufferedImage(width, height, BufferedImage.TYPE_INT_RGB);
        int[] row = new int[width];
        java.util.Arrays.fill(row, BACKGROUND_RGB);
        for (int y = 0; y < height; y++) {
            out.setRGB(0, y, width, 1, row, 0, width);
        }
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            ZipEntry entry = e.nextElement();
            Matcher m = TILE.matcher(entry.getName());
            if (!m.matches() || Integer.parseInt(m.group(1)) != level) {
                continue;
            }
            BufferedImage tile;
            try (InputStream in = zip.getInputStream(entry)) {
                tile = ImageIO.read(in);
            }
            if (tile == null) {
                continue;
            }
            int tw = tile.getWidth();
            int th = tile.getHeight();
            int ox = Integer.parseInt(m.group(2)) * tw;
            int oy = Integer.parseInt(m.group(3)) * th;
            int w = Math.min(tw, width - ox);
            int h = Math.min(th, height - oy);
            if (w <= 0 || h <= 0) {
                continue;
            }
            int[] src = tile.getRGB(0, 0, w, h, null, 0, w);
            int[] dst = out.getRGB(ox, oy, w, h, null, 0, w);
            for (int i = 0; i < src.length; i++) {
                dst[i] = over(src[i], dst[i]);
            }
            out.setRGB(ox, oy, w, h, dst, 0, w);
        }
        return out;
    }

    /** ARGB {@code src} over opaque RGB {@code dst}. */
    static int over(int src, int dst) {
        int a = src >>> 24;
        if (a == 255) return src & 0xFFFFFF;
        if (a == 0) return dst & 0xFFFFFF;
        int r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * (255 - a)) / 255;
        int g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * (255 - a)) / 255;
        int b = ((src & 0xFF) * a + (dst & 0xFF) * (255 - a)) / 255;
        return (r << 16) | (g << 8) | b;
    }

    static byte[] encodeJpeg(BufferedImage image, float quality) throws IOException {
        Iterator<ImageWriter> writers = ImageIO.getImageWritersByFormatName("jpeg");
        if (!writers.hasNext()) {
            throw new IOException("no JPEG writer in this JVM");
        }
        ImageWriter writer = writers.next();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(4 << 20);
        try (ImageOutputStream out = ImageIO.createImageOutputStream(bytes)) {
            writer.setOutput(out);
            ImageWriteParam param = writer.getDefaultWriteParam();
            param.setCompressionMode(ImageWriteParam.MODE_EXPLICIT);
            param.setCompressionQuality(quality);
            writer.write(null, new IIOImage(image, null, null), param);
        } finally {
            writer.dispose();
        }
        return bytes.toByteArray();
    }

    private static int[] ints(String text, int count) {
        String[] parts = text.trim().split("\\s+");
        if (parts.length != count) {
            throw new IllegalArgumentException("expected " + count + " numbers in '" + text + "'");
        }
        int[] out = new int[count];
        for (int i = 0; i < count; i++) {
            out[i] = Integer.parseInt(parts[i]);
        }
        return out;
    }
}
