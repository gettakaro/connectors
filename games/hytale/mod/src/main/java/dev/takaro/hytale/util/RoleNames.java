package dev.takaro.hytale.util;

/**
 * Player-facing names for NPC roles, shared by listEntities, entity-killed and player-death so
 * Takaro sees one name per creature everywhere.
 *
 * <p>Order: the game's own translation (npcRoles.&lt;Role&gt;.name), the role builder's display
 * name, the translated base creature plus the variant part, and only then a humanised id.
 */
public final class RoleNames {
    private RoleNames() {
    }

    /** How a name was found; listEntities logs the counts. */
    public enum Source { TRANSLATION, BUILDER, VARIANT, HUMANISED }

    /** A role's display name, its short description (may be null), and where the name came from. */
    public record Resolved(String name, String description, Source source) {
    }

    public static Resolved resolve(String roleName) {
        String displayName = translate(roleName);
        Source source = displayName != null ? Source.TRANSLATION : null;
        String description = null;
        try {
            com.hypixel.hytale.server.npc.NPCPlugin npc = com.hypixel.hytale.server.npc.NPCPlugin.get();
            com.hypixel.hytale.server.npc.asset.builder.BuilderInfo info =
                npc == null ? null : npc.getRoleBuilderInfo(npc.getIndex(roleName));
            if (info != null && info.getBuilder() instanceof com.hypixel.hytale.server.npc.role.builders.BuilderRole role) {
                String[] names = role.getDisplayNames();
                if (displayName == null && names != null && names.length > 0 && names[0] != null && !names[0].isEmpty()) {
                    displayName = names[0];
                    source = Source.BUILDER;
                }
                String shortDescription = role.getShortDescription();
                if (shortDescription != null && !shortDescription.isEmpty()) {
                    description = shortDescription;
                }
            }
        } catch (Exception e) {
            // Keep going with the other sources rather than dropping the name.
        }
        if (displayName == null || displayName.isEmpty()) {
            // Most behaviour variants (Skeleton_Sand_Mage_Patrol) have no translation of their
            // own, but their base creature does: use the game's name for it.
            for (String base : Catalog.roleBaseCandidates(roleName)) {
                String baseName = translate(base);
                if (baseName != null) {
                    displayName = Catalog.variantName(baseName, base, roleName);
                    source = Source.VARIANT;
                    break;
                }
            }
        }
        if (displayName == null || displayName.isEmpty()) {
            displayName = Catalog.humanise(roleName);
            source = Source.HUMANISED;
        }
        return new Resolved(displayName, description, source);
    }

    /** The display name only. */
    public static String displayName(String roleName) {
        return resolve(roleName).name();
    }

    /** The game's translation of a role name, or null when it has none. */
    public static String translate(String roleName) {
        for (String key : Catalog.roleNameKeys(roleName)) {
            try {
                String translated = com.hypixel.hytale.server.core.util.MessageUtil.formatMessageToPlainString(
                    com.hypixel.hytale.server.core.Message.translation(key).getFormattedMessage());
                if (translated == null) {
                    continue;
                }
                translated = translated.trim();
                if (translated.isEmpty() || translated.equals(key)) {
                    continue;
                }
                return translated;
            } catch (Exception e) {
                // Try the next key form.
            }
        }
        return null;
    }
}
