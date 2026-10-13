package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.ProtocolException;
import io.takaro.zomboid.core.model.MapDimension;
import io.takaro.zomboid.core.model.MapImageChunk;
import io.takaro.zomboid.core.model.MapImageInfo;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The protocol v1 map: one Dimension whose image is the server's own world-map raster
 * ({@code media/maps/<map>/pyramid.zip}, see {@link PyramidMap}), so every server gets the map
 * of its own install and nobody uploads anything.
 *
 * <p>Project Zomboid positions are squares with x growing east and y growing south; the floor
 * level is z and is not part of the map plane. The JPEG is built once on a background thread
 * (a few seconds, ~80 MB transient heap) and cached as {@code <Takaro dir>/map/<revision>.jpg},
 * so a restart only re-hashes the zip. Until it is ready no map is declared.
 */
public final class MapService {

    static final String MIME = "image/jpeg";
    static final float JPEG_QUALITY = 0.88f;
    private static final String BASE_MAP = "Muldraugh, KY";

    /** The loaded map; null until ready. */
    record Loaded(MapDimension dimension, byte[] image) {
    }

    private volatile Loaded loaded;

    public List<MapDimension> dimensions() {
        Loaded current = loaded;
        return current == null ? List.of() : List.of(current.dimension());
    }

    public MapImageChunk read(String dimension, long offset, int length) {
        Loaded current = loaded;
        if (current == null || !current.dimension().id().equals(dimension)) {
            throw new ProtocolException(ProtocolException.NOT_FOUND, "No map image for dimension " + dimension);
        }
        return chunk(current, offset, length);
    }

    static MapImageChunk chunk(Loaded current, long offset, int length) {
        byte[] image = current.image();
        if (offset >= image.length) {
            throw new ProtocolException(ProtocolException.INVALID_ARGS,
                    "offset " + offset + " is past the end of the " + image.length + "-byte image");
        }
        int from = (int) offset;
        int to = (int) Math.min((long) image.length, offset + length);
        byte[] data = java.util.Arrays.copyOfRange(image, from, to);
        return new MapImageChunk(current.dimension().image().revision(), data);
    }

    /** Builds the map on a low-priority daemon thread, then runs {@code onReady}. */
    public void startAsync(Runnable onReady) {
        Thread t = new Thread(() -> {
            try {
                Path pyramid = findPyramid(installDir(), serverMapOption());
                if (pyramid == null) {
                    AgentLog.log("map: no media/maps/<map>/pyramid.zip found; no map declared");
                    return;
                }
                long started = System.currentTimeMillis();
                loaded = load(pyramid, TakaroPaths.dataDir().resolve("map"));
                MapImageInfo image = loaded.dimension().image();
                AgentLog.log("map: ready from " + pyramid + " revision=" + image.revision() + " "
                        + image.width() + "x" + image.height() + " " + image.size() + " bytes in "
                        + (System.currentTimeMillis() - started) + " ms");
                onReady.run();
            } catch (Throwable e) {
                AgentLog.log("WARN map: could not build the map image (" + e + "); no map declared");
            }
        }, "takaro-map");
        t.setDaemon(true);
        t.setPriority(Thread.MIN_PRIORITY);
        t.start();
    }

    /** Loads the cached JPEG for this zip's revision, or renders and caches it. */
    static Loaded load(Path pyramidZip, Path cacheDir) throws IOException {
        String folder = pyramidZip.getParent() != null && pyramidZip.getParent().getFileName() != null
                ? pyramidZip.getParent().getFileName().toString() : BASE_MAP;
        String revision = "pz-" + sha256Prefix(pyramidZip) + "-q" + Math.round(JPEG_QUALITY * 100);
        try (ZipFile zip = new ZipFile(pyramidZip.toFile())) {
            PyramidMap.Info info;
            ZipEntry txt = zip.getEntry("pyramid.txt");
            if (txt == null) throw new IOException("pyramid.zip has no pyramid.txt");
            try (InputStream in = zip.getInputStream(txt)) {
                info = PyramidMap.parseInfo(new String(in.readAllBytes(), StandardCharsets.UTF_8));
            }
            Path cached = cacheDir.resolve(revision + ".jpg");
            Path meta = cacheDir.resolve(revision + ".size");
            byte[] jpeg;
            int width;
            int height;
            if (Files.isRegularFile(cached) && Files.isRegularFile(meta)) {
                jpeg = Files.readAllBytes(cached);
                String[] wh = Files.readString(meta).trim().split("x");
                width = Integer.parseInt(wh[0]);
                height = Integer.parseInt(wh[1]);
            } else {
                PyramidMap.Rendered rendered = PyramidMap.render(zip, JPEG_QUALITY);
                jpeg = rendered.jpeg();
                width = rendered.width();
                height = rendered.height();
                writeCache(cacheDir, cached, meta, jpeg, width + "x" + height);
            }
            String[] idName = dimensionIdName(folder);
            MapDimension dimension = new MapDimension(idName[0], idName[1],
                    "x", "right", "y", "down",
                    info.minX(), info.maxX(), info.minY(), info.maxY(),
                    new MapImageInfo(revision, jpeg.length, width, height, MIME));
            return new Loaded(dimension, jpeg);
        }
    }

    private static void writeCache(Path dir, Path cached, Path meta, byte[] jpeg, String size) {
        try {
            Files.createDirectories(dir);
            Path tmp = Files.createTempFile(dir, "map", ".tmp");
            Files.write(tmp, jpeg);
            Files.move(tmp, cached, StandardCopyOption.REPLACE_EXISTING);
            Files.writeString(meta, size);
        } catch (IOException e) {
            AgentLog.log("WARN map: could not cache the map image (" + e.getMessage() + ")");
        }
    }

    /** Dimension id and name: Knox County for the vanilla map, else the map folder. */
    static String[] dimensionIdName(String mapFolder) {
        if (BASE_MAP.equals(mapFolder)) {
            return new String[] {"knox-county", "Knox County"};
        }
        String id = mapFolder.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "-").replaceAll("^-+|-+$", "");
        if (id.isEmpty()) id = "world";
        if (id.length() > 64) id = id.substring(0, 64);
        String name = mapFolder.length() > 64 ? mapFolder.substring(0, 64) : mapFolder;
        return new String[] {id, name};
    }

    /**
     * The first map of the server's {@code Map=} option (a {@code ;} list, mods first, the
     * vanilla map last) that has a {@code pyramid.zip} in the install, else the vanilla map.
     */
    static Path findPyramid(Path installDir, String mapOption) {
        if (installDir == null) return null;
        List<String> names = new ArrayList<>();
        if (mapOption != null) {
            for (String n : mapOption.split(";")) {
                if (!n.trim().isEmpty()) names.add(n.trim());
            }
        }
        if (!names.contains(BASE_MAP)) names.add(BASE_MAP);
        for (String name : names) {
            Path zip = installDir.resolve("media").resolve("maps").resolve(name).resolve("pyramid.zip");
            if (Files.isRegularFile(zip)) return zip;
        }
        return null;
    }

    /** The install folder: two levels above {@code java/projectzomboid.jar}, else the working dir. */
    static Path installDir() {
        Path jar = TargetGuard.findGameJar();
        if (jar != null) {
            Path abs = jar.toAbsolutePath();
            if (abs.getParent() != null && abs.getParent().getParent() != null) {
                return abs.getParent().getParent();
            }
        }
        return Paths.get(System.getProperty("user.dir", "."));
    }

    /** {@code ServerOptions.instance.getOption("Map")}, read reflectively; null if unavailable. */
    static String serverMapOption() {
        try {
            Class<?> options = Class.forName("zombie.network.ServerOptions");
            Object instance = options.getField("instance").get(null);
            Object value = options.getMethod("getOption", String.class).invoke(instance, "Map");
            return value != null ? value.toString() : null;
        } catch (Throwable t) {
            return null;
        }
    }

    static String sha256Prefix(Path file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = Files.newInputStream(file)) {
                byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    digest.update(buffer, 0, read);
                }
            }
            return HexFormat.of().formatHex(digest.digest()).substring(0, 16);
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }
}
