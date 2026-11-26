package dev.takaro.hytale.protocol;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The protocol v1 Capability Manifest: only what the plugin really answers and emits.
 *
 * <p>Hytale has no levels or skills API the connector could read, so neither
 * {@code players.progression} nor {@code players.characters} is declared.
 */
public final class Capabilities {

    /** Action capabilities. testReachability needs none. */
    public static final List<String> ACTIONS = List.of(
        "players.list",
        "players.get",
        "players.location",
        "players.inventory",
        "players.teleport",
        "items.list",
        "items.give",
        "entities.list",
        "locations.list",
        "chat.send",
        "console.execute",
        "moderation.kick",
        "moderation.ban",
        "moderation.unban",
        "moderation.listBans",
        "server.shutdown");

    /** One descriptive entry per event type the plugin emits. */
    public static final List<String> EVENTS = List.of(
        "events.player-connected",
        "events.player-disconnected",
        "events.chat-message",
        "events.player-death",
        "events.entity-killed",
        "events.log");

    public static final String GAME_PROFILE = "game.profile";
    public static final String MAP = "map";

    private Capabilities() {
    }

    /**
     * The full manifest. {@code mapDimension} is one Dimension declaration, or null while the
     * world map is off or not ready (the map capability is then left out, and added later with a
     * full capabilities replacement).
     */
    public static List<Map<String, Object>> manifest(String rawGameVersion, Map<String, Object> mapDimension) {
        List<Map<String, Object>> out = new ArrayList<>();
        for (String name : ACTIONS) {
            out.add(named(name));
        }
        for (String name : EVENTS) {
            out.add(named(name));
        }
        Map<String, Object> profile = named(GAME_PROFILE);
        profile.put("parameters", GameProfile.parameters(rawGameVersion));
        out.add(profile);
        if (mapDimension != null) {
            Map<String, Object> map = named(MAP);
            Map<String, Object> parameters = new LinkedHashMap<>();
            parameters.put("dimensions", List.of(mapDimension));
            map.put("parameters", parameters);
            out.add(map);
        }
        return out;
    }

    private static Map<String, Object> named(String name) {
        Map<String, Object> capability = new LinkedHashMap<>();
        capability.put("name", name);
        return capability;
    }
}
