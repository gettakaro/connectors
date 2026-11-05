package io.takaro.minecraft.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The last XP each player was seen with. The platform records it on the game thread when a player
 * joins or leaves and once a second in between, and Takaro reads it on every player sync, so
 * answering never waits for a server tick. A player keeps their last reading after leaving, until
 * the server restarts.
 */
public final class PlayerProgression {

    private record Reading(int level, float bar, int experience) {}

    private final Map<String, Reading> readings = new ConcurrentHashMap<>();

    /**
     * @param level      the experience level above the hotbar
     * @param bar        how full the XP bar is
     * @param experience the game's total points, kept since the player last died; spending levels
     *                   on enchanting does not lower it
     */
    public void record(String gameId, int level, float bar, int experience) {
        readings.put(gameId, new Reading(level, bar, experience));
    }

    /** The player's tracks, or null for a player this server has not seen since it started. */
    JsonArray tracks(String gameId) {
        Reading reading = readings.get(gameId);
        if (reading == null) return null;
        JsonObject character = new JsonObject();
        character.addProperty("key", "character");
        character.addProperty("name", "Level");
        character.addProperty("level", reading.level());
        character.addProperty("experience", reading.experience());
        character.addProperty("progress", progress(reading.bar()));
        JsonArray tracks = new JsonArray();
        tracks.add(character);
        return tracks;
    }

    /** Takaro rejects the whole reading for a progress outside 0..1, so a stray bar value cannot pass. */
    private static double progress(float bar) {
        if (!(bar > 0)) return 0;
        return Math.min(1, Math.round(bar * 10000d) / 10000d);
    }
}
