package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.ProgressionTrack;

/**
 * Turns one Project Zomboid perk into a Progression Track. Links no game classes, so it stays
 * unit-testable. Takaro rejects a whole reading for one bad track, and mods can add perks with any
 * id, so an id that cannot become a valid key yields no track rather than a bad one.
 */
final class PerkTracks {

    static final int MAX_TRACKS = 200;
    private static final int MAX_TEXT = 128;

    private PerkTracks() {
    }

    /** A track key from a perk id ({@code LongBlade} to {@code longBlade}), or null if there is none. */
    static String key(String perkId) {
        if (perkId == null) {
            return null;
        }
        StringBuilder out = new StringBuilder();
        boolean upperNext = false;
        for (int i = 0; i < perkId.length(); i++) {
            char c = perkId.charAt(i);
            boolean letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!letter && !(c >= '0' && c <= '9')) {
                upperNext = out.length() > 0;
                continue;
            }
            if (out.length() == 0) {
                if (!letter) {
                    continue;
                }
                out.append(Character.toLowerCase(c));
            } else {
                out.append(upperNext ? Character.toUpperCase(c) : c);
            }
            upperNext = false;
        }
        String key = out.toString();
        // "character" is reserved for one primary level, which Project Zomboid does not have.
        if (key.isEmpty() || key.length() > MAX_TEXT || key.equals("character")) {
            return null;
        }
        return key;
    }

    /**
     * The track for a perk, or null when its id gives no valid key.
     *
     * @param xp           the perk's experience
     * @param xpAtLevel    experience at which the current level starts
     * @param xpToNext     experience the next level takes; zero or less at the top level
     */
    static ProgressionTrack track(String perkId, String name, int level, float xp, float xpAtLevel, float xpToNext) {
        String key = key(perkId);
        if (key == null) {
            return null;
        }
        boolean finite = Float.isFinite(xp);
        Double progress = null;
        if (finite && xpToNext > 0) {
            progress = Math.max(0.0, Math.min(1.0, ((double) xp - xpAtLevel) / xpToNext));
        }
        // Via the shortest float text, so 12.3f goes out as 12.3 and not 12.300000190734863.
        Double experience = finite ? Double.valueOf(Float.toString(xp)) : null;
        return new ProgressionTrack(key, displayName(name), level, experience, progress);
    }

    private static String displayName(String name) {
        if (name == null || name.isBlank()) {
            return null;
        }
        String trimmed = name.trim();
        return trimmed.length() > MAX_TEXT ? trimmed.substring(0, MAX_TEXT) : trimmed;
    }
}
