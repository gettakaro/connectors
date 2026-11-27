package dev.takaro.hytale.protocol;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.takaro.hytale.map.MapPyramid;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class ProtocolV1Test {

    private static final Gson GSON = new Gson();

    private static JsonObject identify(Map<String, Object> mapDimension) {
        return JsonParser.parseString(GSON.toJson(IdentifyPayload.build("0.1.0", "2026.01.24-6e2d4fc36", mapDimension)))
            .getAsJsonObject();
    }

    private static List<String> names(JsonArray capabilities) {
        List<String> out = new ArrayList<>();
        for (JsonElement c : capabilities) {
            out.add(c.getAsJsonObject().get("name").getAsString());
        }
        return out;
    }

    private static JsonObject capability(JsonArray capabilities, String name) {
        for (JsonElement c : capabilities) {
            if (c.getAsJsonObject().get("name").getAsString().equals(name)) {
                return c.getAsJsonObject();
            }
        }
        return null;
    }

    @Test
    void identifyOffersBothVersionsAndNamesGameAndConnector() {
        JsonObject p = identify(null);
        assertEquals("[0,1]", p.get("protocolVersions").toString());
        assertEquals("hytale", p.get("game").getAsString());
        assertEquals("takaro-hytale", p.getAsJsonObject("connector").get("name").getAsString());
        assertEquals("0.1.0", p.getAsJsonObject("connector").get("version").getAsString());
    }

    @Test
    void identifyWithoutAUsableVersionLeavesConnectorOut() {
        Map<String, Object> p = IdentifyPayload.build("dev", null, null);
        assertFalse(p.containsKey("connector"));
        assertEquals("hytale", p.get("game"));
    }

    @Test
    void manifestDeclaresExactlyWhatThePluginAnswers() {
        JsonArray caps = identify(null).getAsJsonArray("capabilities");
        assertEquals(List.of(
            "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
            "items.list", "items.give", "entities.list", "locations.list", "chat.send", "console.execute",
            "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown",
            "events.player-connected", "events.player-disconnected", "events.chat-message",
            "events.player-death", "events.entity-killed", "events.log",
            "game.profile"), names(caps));
        // Hytale has no levels or skills API to report.
        assertFalse(names(caps).contains("players.progression"));
        assertFalse(names(caps).contains("players.characters"));
        assertFalse(names(caps).contains("map"), "no map before it is ready");
    }

    @Test
    void gameProfileIsInTheManifest() {
        JsonObject profile = capability(identify(null).getAsJsonArray("capabilities"), "game.profile");
        JsonObject parameters = profile.getAsJsonObject("parameters");
        assertEquals("Hytale", parameters.get("name").getAsString());
        assertEquals("2026.01.24-6e2d4fc36", parameters.get("version").getAsString());
        assertEquals(GameProfile.icon().revision(), parameters.getAsJsonObject("icon").get("revision").getAsString());
        assertEquals(GameProfile.icon().size(), parameters.getAsJsonObject("icon").get("size").getAsInt());
    }

    @Test
    void mapIsOneTiledDimension() {
        MapPyramid.Frame frame = MapPyramid.defaultFrame();
        JsonArray caps = identify(MapPyramid.declaration("default", "default", frame)).getAsJsonArray("capabilities");
        JsonObject map = capability(caps, "map");
        assertNotNull(map);
        JsonArray dims = map.getAsJsonObject("parameters").getAsJsonArray("dimensions");
        assertEquals(1, dims.size());
        JsonObject dim = dims.get(0).getAsJsonObject();
        assertEquals("default", dim.get("id").getAsString());
        JsonObject source = dim.getAsJsonObject("source");
        assertEquals("tiled", source.get("type").getAsString());
        assertEquals(256, source.get("tileSize").getAsInt());
        assertEquals(0, source.get("minZoom").getAsInt());
        assertEquals(frame.maxZoom(), source.get("maxZoom").getAsInt());
    }

    @Test
    void chosenProtocolDefaultsToZero() {
        assertEquals(0, IdentifyPayload.chosenProtocol(null));
        assertEquals(0, IdentifyPayload.chosenProtocol(new JsonObject()));
        assertEquals(0, IdentifyPayload.chosenProtocol(JsonParser.parseString("{\"protocolVersion\":0}").getAsJsonObject()));
        assertEquals(1, IdentifyPayload.chosenProtocol(JsonParser.parseString("{\"protocolVersion\":1}").getAsJsonObject()));
        assertEquals(0, IdentifyPayload.chosenProtocol(JsonParser.parseString("{\"protocolVersion\":7}").getAsJsonObject()));
        assertEquals(0, IdentifyPayload.chosenProtocol(JsonParser.parseString("{\"protocolVersion\":\"1\"}").getAsJsonObject()));
        assertEquals(0, IdentifyPayload.chosenProtocol(JsonParser.parseString("{\"protocolVersion\":null}").getAsJsonObject()));
    }

    // --- response mapping ---

    private static Map<String, Object> failure(String error) {
        Map<String, Object> m = new java.util.HashMap<>();
        m.put("success", false);
        if (error != null) {
            m.put("error", error);
        }
        return m;
    }

    private static String codeOf(String action, Object result) {
        ProtocolException e = assertThrows(ProtocolException.class, () -> V1Results.adapt(action, result));
        return e.code();
    }

    @Test
    void voidActionsAnswerNullOnSuccess() {
        for (String action : List.of("sendMessage", "giveItem", "kickPlayer", "banPlayer", "unbanPlayer",
            "teleportPlayer", "shutdown")) {
            assertNull(V1Results.adapt(action, Map.of("success", true)), action);
        }
        assertNull(V1Results.adapt("giveItem", Map.of("success", true, "qualityIgnored", "3")));
        assertNull(V1Results.adapt("shutdown", Map.of("success", true, "rawResult", "Server shutdown initiated")));
    }

    @Test
    void failuresBecomeErrorCodes() {
        assertEquals("invalid_args", codeOf("kickPlayer", failure("No gameId or playerId provided")));
        assertEquals("invalid_args", codeOf("banPlayer", failure("gameId is not a Hytale UUID: abc")));
        assertEquals("invalid_args", codeOf("banPlayer", failure("Could not parse expiresAt: soon")));
        assertEquals("invalid_args", codeOf("teleportPlayer", failure("teleportPlayer needs numeric x, y and z")));
        assertEquals("not_found", codeOf("kickPlayer", failure("Player not found")));
        assertEquals("not_found", codeOf("giveItem", failure("Player not in world")));
        assertEquals("not_found", codeOf("getPlayerInventory", failure("Player is not online: x")));
        assertEquals("game_error", codeOf("giveItem", failure("Item could not be added (unknown item id, or inventory full)")));
        assertEquals("game_error", codeOf("sendMessage", failure(null)));
        assertEquals("game_error", codeOf("kickPlayer", failure("Universe is null")));
        assertEquals("unsupported", codeOf("getMapTile", failure("map_disabled: MAP_ENABLED=false")));
    }

    @Test
    void errorMessageIsCarriedOver() {
        ProtocolException e = assertThrows(ProtocolException.class,
            () -> V1Results.adapt("kickPlayer", failure("Player not found")));
        assertEquals("Player not found", e.getMessage());
        e = assertThrows(ProtocolException.class, () -> V1Results.adapt("sendMessage", failure(null)));
        assertEquals("sendMessage failed", e.getMessage());
    }

    @Test
    void anUnknownPlayerIsNullForGetPlayerAndGetPlayerLocation() {
        assertNull(V1Results.adapt("getPlayerLocation", failure("Player is not online: 1234")));
        assertNull(V1Results.adapt("getPlayerLocation", failure("Player is not in a world: 1234")));
        assertNull(V1Results.adapt("getPlayer", failure("Player not found (no gameId, and the name has never been seen)")));
        assertNull(V1Results.adapt("getPlayer", null));
        // Bad arguments still fail.
        assertEquals("invalid_args", codeOf("getPlayerLocation", failure("No gameId or playerId provided")));
    }

    @Test
    void commandOutputPassesThroughEvenWhenTheCommandFailed() {
        Map<String, Object> out = Map.of("success", false, "rawResult", "Command not found: foo");
        assertSame(out, V1Results.adapt("executeConsoleCommand", out));
    }

    @Test
    void dataResultsPassThrough() {
        Map<String, Object> location = Map.of("x", 1.0, "y", 2.0, "z", 3.0);
        assertSame(location, V1Results.adapt("getPlayerLocation", location));
        List<Object> players = List.of();
        assertSame(players, V1Results.adapt("getPlayers", players));
        Map<String, Object> reach = new java.util.HashMap<>();
        reach.put("connectable", true);
        reach.put("reason", null);
        assertSame(reach, V1Results.adapt("testReachability", reach));
    }

    @Test
    void escapingExceptionsGetACode() {
        assertEquals("not_found", ProtocolException.codeFor(new ProtocolException("not_found", "x")));
        assertEquals("invalid_args", ProtocolException.codeFor(new IllegalArgumentException("bad uuid")));
        assertEquals("unsupported", ProtocolException.codeFor(new UnsupportedOperationException()));
        assertEquals("game_error", ProtocolException.codeFor(new IllegalStateException()));
    }
}
