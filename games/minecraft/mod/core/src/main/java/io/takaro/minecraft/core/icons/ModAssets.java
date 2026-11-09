package io.takaro.minecraft.core.icons;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** The {@code assets/} trees of the mods on this server. The first root holding a file wins. */
final class ModAssets {

    private final List<Path> roots;
    private final Map<String, List<Path>> rootsByNamespace = new ConcurrentHashMap<>();

    ModAssets(List<Path> roots) {
        this.roots = List.copyOf(roots);
    }

    /** The file {@code assets/<namespace>/<relative>}, or null when no mod has it. */
    Path find(String namespace, String relative) {
        for (Path root : rootsWith(namespace)) {
            Path file = root.resolve("assets").resolve(namespace).resolve(relative);
            if (Files.isRegularFile(file)) {
                return file;
            }
        }
        return null;
    }

    /** The JSON object in that file, or null when it is absent or unreadable. */
    JsonObject json(String namespace, String relative) {
        Path file = find(namespace, relative);
        if (file == null) {
            return null;
        }
        try {
            JsonElement parsed = JsonParser.parseString(Files.readString(file, StandardCharsets.UTF_8));
            return parsed.isJsonObject() ? parsed.getAsJsonObject() : null;
        } catch (IOException | JsonParseException e) {
            return null;
        }
    }

    // A server has hundreds of roots and thousands of items, and most roots hold one namespace.
    private List<Path> rootsWith(String namespace) {
        return rootsByNamespace.computeIfAbsent(namespace, ns -> roots.stream()
                .filter(root -> Files.isDirectory(root.resolve("assets").resolve(ns)))
                .toList());
    }
}
