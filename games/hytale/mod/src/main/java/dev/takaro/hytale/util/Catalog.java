package dev.takaro.hytale.util;

import java.util.Locale;

/**
 * Catalogue hygiene rules shared by listItems and listEntities.
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar.
 */
public final class Catalog {
    private Catalog() {
    }

    /** Code segments that mark an asset as developer scaffolding or an abstract parent. */
    private static final java.util.Set<String> DEVELOPER_TOKENS = java.util.Set.of("debug", "test", "dev", "template");

    /**
     * True for codes that are developer scaffolding or abstract parents rather than real game
     * content, e.g. {@code Debug_Donut_Selector}, {@code Test_Block}, {@code Dev_Marker},
     * and, anywhere in the code, {@code Glass_Test_Red}, {@code Weapon_Test_Zoom_Rifle},
     * {@code Template_Weapon_Sword}, {@code Potion_Template} (0.7.7 ships 37 of these).
     *
     * <p>Only a whole {@code _}/{@code -} separated segment counts, so a real item that merely
     * contains the word ({@code Tool_Debugger}, {@code Device_Panel}) stays in.
     */
    public static boolean isDeveloperAsset(String code) {
        if (code == null) {
            return false;
        }
        for (String segment : code.toLowerCase(Locale.ROOT).split("[_\\-]+")) {
            if (DEVELOPER_TOKENS.contains(segment)) {
                return true;
            }
        }
        return false;
    }

    /**
     * The shorter role ids an NPC variant can be named after, longest first:
     * {@code Skeleton_Sand_Mage_Patrol} gives {@code Skeleton_Sand_Mage}, {@code Skeleton_Sand},
     * {@code Skeleton}. Hytale translates the base creature but not most behaviour variants.
     */
    public static java.util.List<String> roleBaseCandidates(String code) {
        java.util.List<String> out = new java.util.ArrayList<>();
        if (code == null) {
            return out;
        }
        String current = code;
        int cut;
        while ((cut = current.lastIndexOf('_')) > 0) {
            current = current.substring(0, cut);
            out.add(current);
        }
        return out;
    }

    /**
     * The display name of a variant: the base creature's translated name plus the variant part
     * of the id, e.g. {@code ("Sandswept Skeleton Mage", "Skeleton_Sand_Mage", "Skeleton_Sand_Mage_Patrol")}
     * gives {@code "Sandswept Skeleton Mage (Patrol)"}.
     */
    public static String variantName(String baseName, String baseCode, String code) {
        String rest = code.substring(baseCode.length()).replaceFirst("^[_\\-]+", "");
        return rest.isEmpty() ? baseName : baseName + " (" + humanise(rest) + ")";
    }

    /**
     * The i18n key Hytale itself uses for an NPC role's player-facing name.
     *
     * <p>Found in {@code Server/Languages/en-US/server.lang} inside {@code Assets.zip} (and in
     * {@code bundledDefaults/server.lang} inside the server jar): 574 entries of the form
     * {@code npcRoles.Bat_Ice.name = Ice Bat}, {@code npcRoles.Cow_Calf.name = Calf}, which
     * I18nModule registers under the file-name prefix as {@code server.npcRoles.Bat_Ice.name}.
     * {@code BuilderRole.getDisplayNames()} does not carry them.
     */
    public static java.util.List<String> roleNameKeys(String code) {
        // I18nModule prefixes every key with the .lang file's own name, so the key that
        // server.lang's `npcRoles.Bat_Ice.name` is actually registered under at runtime is
        // `server.npcRoles.Bat_Ice.name` (the same way HelpCommand asks for
        // `server.commands.help.console.header` for a line written as
        // `commands.help.console.header`). The unprefixed form is tried too, in case a future
        // asset pack ships the table under a different file name.
        return java.util.List.of("server.npcRoles." + code + ".name", "npcRoles." + code + ".name");
    }

    /**
     * True for role templates that are engine scaffolding rather than creatures a server owner
     * would ever want in a Takaro entity list: the {@code Static}/{@code Static2..4} marker
     * roles, {@code Template} / {@code BlankTemplate}, and {@code Empty_Role}.
     */
    public static boolean isPlaceholderRole(String code) {
        if (code == null || code.isEmpty()) {
            return true;
        }
        if (code.equals("Template") || code.equals("BlankTemplate") || code.equals("Empty_Role")) {
            return true;
        }
        // Static, Static2, Static3, ...
        return code.startsWith("Static")
            && code.substring("Static".length()).chars().allMatch(Character::isDigit);
    }

    /**
     * Last-resort humanisation of a role id when the game has no translation for it:
     * {@code Rex_Cave} becomes {@code "Rex Cave"}, {@code trork_grunt} becomes
     * {@code "Trork Grunt"}. Deliberately does not try to reorder words - inventing
     * {@code "Cave Rex"} when the game never said so would be a guess dressed as a name.
     */
    public static String humanise(String code) {
        if (code == null || code.isEmpty()) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        for (String word : code.split("[_\\-]+")) {
            if (word.isEmpty()) {
                continue;
            }
            if (out.length() > 0) {
                out.append(' ');
            }
            out.append(Character.toUpperCase(word.charAt(0)));
            if (word.length() > 1) {
                out.append(word.substring(1));
            }
        }
        return out.length() == 0 ? code : out.toString();
    }
}
