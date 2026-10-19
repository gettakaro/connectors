package io.takaro.zomboid.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import io.takaro.zomboid.core.model.MapDimension;
import io.takaro.zomboid.core.model.MapImageInfo;

import java.util.List;

/**
 * The protocol v1 Capability Manifest: the actions this connector really answers, the
 * events it emits, the {@code game.profile} capability, and the {@code map} capability when
 * the game has a map to declare.
 */
public final class Capabilities {

    /** Action capabilities, in the protocol's order. testReachability is not a capability. */
    static final List<String> ACTIONS = List.of(
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

    /** Descriptive event entries: one per event type the connector emits. */
    static final List<String> EVENTS = List.of(
            "events.player-connected",
            "events.player-disconnected",
            "events.chat-message",
            "events.player-death",
            "events.entity-killed",
            "events.log");

    static final String GAME_PROFILE = "game.profile";

    private Capabilities() {
    }

    public static JsonArray manifest(List<MapDimension> dimensions) {
        return manifest(dimensions, null);
    }

    public static JsonArray manifest(List<MapDimension> dimensions, String gameVersion) {
        JsonArray out = new JsonArray();
        for (String name : ACTIONS) {
            out.add(named(name));
        }
        for (String name : EVENTS) {
            out.add(named(name));
        }
        JsonObject profile = named(GAME_PROFILE);
        profile.add("parameters", GameProfile.parameters(gameVersion));
        out.add(profile);
        if (dimensions != null && !dimensions.isEmpty()) {
            JsonObject map = named("map");
            JsonObject parameters = new JsonObject();
            JsonArray dims = new JsonArray();
            for (MapDimension d : dimensions) {
                dims.add(dimensionJson(d));
            }
            parameters.add("dimensions", dims);
            map.add("parameters", parameters);
            out.add(map);
        }
        return out;
    }

    static JsonObject dimensionJson(MapDimension d) {
        JsonObject dim = new JsonObject();
        dim.addProperty("id", d.id());
        dim.addProperty("name", d.name());

        JsonObject axes = new JsonObject();
        axes.add("horizontal", axis(d.horizontalField(), d.horizontalDirection()));
        axes.add("vertical", axis(d.verticalField(), d.verticalDirection()));
        dim.add("axes", axes);

        JsonObject bounds = new JsonObject();
        bounds.add("horizontal", range(d.horizontalMin(), d.horizontalMax()));
        bounds.add("vertical", range(d.verticalMin(), d.verticalMax()));
        dim.add("bounds", bounds);

        JsonObject source = new JsonObject();
        MapImageInfo image = d.image();
        if (image == null) {
            source.addProperty("type", "none");
        } else {
            source.addProperty("type", "image");
            source.addProperty("revision", image.revision());
            source.addProperty("size", image.size());
            source.addProperty("width", image.width());
            source.addProperty("height", image.height());
            source.addProperty("mimeType", image.mimeType());
        }
        dim.add("source", source);
        return dim;
    }

    private static JsonObject named(String name) {
        JsonObject capability = new JsonObject();
        capability.addProperty("name", name);
        return capability;
    }

    private static JsonObject axis(String field, String direction) {
        JsonObject axis = new JsonObject();
        axis.addProperty("field", field);
        axis.addProperty("direction", direction);
        return axis;
    }

    private static JsonObject range(double min, double max) {
        JsonObject range = new JsonObject();
        range.addProperty("min", min);
        range.addProperty("max", max);
        return range;
    }
}
