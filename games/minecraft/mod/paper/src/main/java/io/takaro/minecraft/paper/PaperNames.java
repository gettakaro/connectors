package io.takaro.minecraft.paper;

import org.bukkit.Material;
import org.bukkit.entity.EntityType;

import java.lang.reflect.Method;
import java.util.function.Function;

/**
 * Display names for the item and entity catalogues Takaro shows to server admins.
 *
 * A registry id ("minecraft:diamond_sword") or a raw translation key ("item.minecraft.diamond_sword")
 * is not a name: Takaro renders these strings in shop listings and module arguments, and the
 * verification harness rejects a name that equals its code or still looks like a key. The
 * translated name is read out of the vanilla server language table when it is reachable, and a
 * title-cased key path is used when it is not.
 */
final class PaperNames {

    /** The vanilla language table, or null when this server does not expose it. */
    private static final Function<String, String> VANILLA = vanillaLookup();

    private PaperNames() {
    }

    static String itemName(Material material) {
        return displayName(itemTranslationKey(material), material.getKey().getKey(), VANILLA);
    }

    static String entityName(EntityType type) {
        return displayName(type.translationKey(), type.getKey().getKey(), VANILLA);
    }

    private static String itemTranslationKey(Material material) {
        try {
            return material.getItemTranslationKey();
        } catch (RuntimeException e) {
            // A material that is not an item has no item key; the block key still names it.
            return material.translationKey();
        }
    }

    /**
     * The translated name when the language table knows the key, otherwise the key path
     * title-cased. Never the registry id, never the key itself.
     */
    static String displayName(String translationKey, String keyPath, Function<String, String> lookup) {
        if (lookup != null && translationKey != null && !translationKey.isEmpty()) {
            String translated = lookup.apply(translationKey);
            if (translated != null && !translated.isEmpty() && !translated.equals(translationKey)) {
                return translated;
            }
        }
        return titleCase(keyPath);
    }

    /** "diamond_sword" -&gt; "Diamond Sword"; a namespace prefix is dropped first. */
    static String titleCase(String keyPath) {
        if (keyPath == null || keyPath.isEmpty()) {
            return "";
        }
        String path = keyPath.substring(keyPath.indexOf(':') + 1).replace('.', '_');
        StringBuilder out = new StringBuilder(path.length());
        boolean boundary = true;
        for (int i = 0; i < path.length(); i++) {
            char ch = path.charAt(i);
            if (ch == '_' || ch == '-' || ch == '/') {
                out.append(' ');
                boundary = true;
                continue;
            }
            out.append(boundary ? Character.toUpperCase(ch) : Character.toLowerCase(ch));
            boundary = false;
        }
        return out.toString().trim();
    }

    /**
     * Paper 1.20.5+ runs Mojang-mapped, so {@code net.minecraft.locale.Language} is reachable by
     * name. Reflection keeps this plugin compiling against the Paper API alone, and every failure
     * path simply leaves the title-cased fallback in charge.
     */
    private static Function<String, String> vanillaLookup() {
        try {
            Class<?> language = Class.forName("net.minecraft.locale.Language");
            Object instance = language.getMethod("getInstance").invoke(null);
            Method getOrDefault = language.getMethod("getOrDefault", String.class);
            return key -> {
                try {
                    Object value = getOrDefault.invoke(instance, key);
                    return value instanceof String text ? text : null;
                } catch (ReflectiveOperationException | RuntimeException e) {
                    return null;
                }
            };
        } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
            return null;
        }
    }
}
