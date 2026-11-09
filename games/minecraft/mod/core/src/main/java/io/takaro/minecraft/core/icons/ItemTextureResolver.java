package io.takaro.minecraft.core.icons;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Follows an item id through the client assets to the texture that draws it: item definition,
 * then model and its parents, then the conventional texture paths.
 */
final class ItemTextureResolver {

    private static final Pattern LOCATION = Pattern.compile("(?:([a-z0-9_.-]+):)?([a-z0-9_./-]+)");
    private static final String VANILLA = "minecraft";
    private static final int MAX_DEPTH = 16;
    /** Item definition branches, the resting state first: a bow's picture is the undrawn bow. */
    private static final List<String> DEFINITION_BRANCHES =
            List.of("fallback", "on_false", "on_true", "model", "cases", "entries", "models");
    private static final List<String> FLAT_TEXTURE_KEYS =
            List.of("layer0", "all", "side", "top", "end", "front", "texture", "cross", "plant");

    private final ModAssets assets;

    ItemTextureResolver(ModAssets assets) {
        this.assets = assets;
    }

    /**
     * The texture file for this item, or null. Vanilla items are skipped: a server jar has no
     * vanilla art, and a mod shipping a vanilla path is a retexture, not this item's icon.
     */
    Path resolve(String itemCode) {
        Location item = Location.parse(itemCode);
        if (item == null || item.namespace().equals(VANILLA)) {
            return null;
        }
        Path texture = fromModel(itemModel(item));
        if (texture != null) {
            return texture;
        }
        for (String folder : List.of("item", "block")) {
            Path file = assets.find(item.namespace(), "textures/" + folder + "/" + item.path() + ".png");
            if (file != null) {
                return file;
            }
        }
        return null;
    }

    private Location itemModel(Location item) {
        JsonObject definition = assets.json(item.namespace(), "items/" + item.path() + ".json");
        String model = definition != null ? modelReference(definition, 0) : null;
        Location parsed = model != null ? Location.parse(model) : null;
        return parsed != null ? parsed : new Location(item.namespace(), "item/" + item.path());
    }

    private static String modelReference(JsonElement node, int depth) {
        if (depth > MAX_DEPTH) {
            return null;
        }
        if (node.isJsonArray()) {
            JsonArray array = node.getAsJsonArray();
            for (JsonElement entry : array) {
                String found = modelReference(entry, depth + 1);
                if (found != null) {
                    return found;
                }
            }
            return null;
        }
        if (!node.isJsonObject()) {
            return null;
        }
        JsonObject object = node.getAsJsonObject();
        for (String key : List.of("model", "base")) {
            JsonElement value = object.get(key);
            if (value != null && value.isJsonPrimitive() && value.getAsJsonPrimitive().isString()) {
                return value.getAsString();
            }
        }
        for (String branch : DEFINITION_BRANCHES) {
            JsonElement value = object.get(branch);
            String found = value != null ? modelReference(value, depth + 1) : null;
            if (found != null) {
                return found;
            }
        }
        return null;
    }

    private Path fromModel(Location model) {
        Map<String, JsonElement> textures = new HashMap<>();
        boolean customGeometry = false;
        Set<Location> seen = new HashSet<>();
        Location current = model;
        while (current != null && seen.size() < MAX_DEPTH && seen.add(current)) {
            JsonObject json = assets.json(current.namespace(), "models/" + current.path() + ".json");
            if (json == null) {
                break; // a vanilla parent: the server jar does not have it
            }
            customGeometry |= json.has("elements");
            JsonElement own = json.get("textures");
            if (own != null && own.isJsonObject()) {
                own.getAsJsonObject().entrySet().forEach(e -> textures.putIfAbsent(e.getKey(), e.getValue()));
            }
            current = stringValue(json.get("parent")) != null ? Location.parse(stringValue(json.get("parent"))) : null;
        }
        for (String key : FLAT_TEXTURE_KEYS) {
            Path file = textureFile(textures, key);
            if (file != null) {
                return file;
            }
        }
        // On a model with its own geometry the textures are UV sheets, which are not a picture.
        return customGeometry ? null : textureFile(textures, "particle");
    }

    private Path textureFile(Map<String, JsonElement> textures, String key) {
        String value = textureValue(textures.get(key));
        for (int hops = 0; value != null && value.startsWith("#") && hops < MAX_DEPTH; hops++) {
            value = textureValue(textures.get(value.substring(1)));
        }
        Location location = value != null && !value.startsWith("#") ? Location.parse(value) : null;
        return location != null ? assets.find(location.namespace(), "textures/" + location.path() + ".png") : null;
    }

    /** A texture is a plain string or, since 1.21.5, an object with a {@code sprite}. */
    private static String textureValue(JsonElement value) {
        if (value != null && value.isJsonObject()) {
            return stringValue(value.getAsJsonObject().get("sprite"));
        }
        return stringValue(value);
    }

    private static String stringValue(JsonElement value) {
        return value != null && value.isJsonPrimitive() && value.getAsJsonPrimitive().isString()
                ? value.getAsString() : null;
    }

    private record Location(String namespace, String path) {
        /** Mod-authored JSON feeds this, so a {@code ..} never reaches a file path. */
        static Location parse(String value) {
            Matcher m = LOCATION.matcher(value);
            if (!m.matches() || m.group(2).contains("..")) {
                return null;
            }
            return new Location(m.group(1) != null ? m.group(1) : VANILLA, m.group(2));
        }
    }
}
