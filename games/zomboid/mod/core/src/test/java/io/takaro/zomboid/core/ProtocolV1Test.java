package io.takaro.zomboid.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.zomboid.core.model.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.net.URI;
import java.util.*;
import java.util.concurrent.CopyOnWriteArrayList;

import static org.junit.jupiter.api.Assertions.*;

/** Protocol v1 against a fake Takaro: handshake, manifest, error shape, event stream, map image. */
class ProtocolV1Test {

    private static final MapDimension DIM = new MapDimension("knox-county", "Knox County",
            "x", "right", "y", "down", 0, 19968, 0, 16128,
            new MapImageInfo("rev-1", 10, 4992, 4032, "image/jpeg"));
    private static final byte[] IMAGE = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    private FakeAdapter adapter;
    private TakaroConfig config;
    private EventOutbox outbox;
    private List<String> sent;
    private boolean open;
    private TakaroWebSocketClient client;

    @BeforeEach
    void setUp() throws Exception {
        adapter = new FakeAdapter();
        config = new TakaroConfig();
        config.setIdentityToken("id");
        config.setRegistrationToken("reg");
        outbox = new EventOutbox("stream-1", 3);
        sent = new CopyOnWriteArrayList<>();
        open = true;
        client = newClient();
    }

    private TakaroWebSocketClient newClient() throws Exception {
        return new TakaroWebSocketClient(new URI("ws://localhost:9999"), adapter, config, outbox) {
            @Override public void send(String text) { sent.add(text); }
            @Override public boolean isOpen() { return open; }
            @Override public void close() { }
        };
    }

    @Test
    void identifyOffersBothVersionsAndTheManifest() {
        client.onOpen(null);
        JsonObject identify = frame(0);
        assertEquals("identify", identify.get("type").getAsString());
        JsonObject payload = identify.getAsJsonObject("payload");
        assertEquals("[0,1]", payload.get("protocolVersions").toString());
        Set<String> names = names(payload.getAsJsonArray("capabilities"));
        assertTrue(names.containsAll(Capabilities.ACTIONS));
        assertTrue(names.containsAll(Capabilities.EVENTS));
        assertFalse(names.contains("map"), "no map before the image is ready");
    }

    @Test
    void protocolSwitchOffSendsTheVersion0Identify() {
        config.setProtocolV1Enabled(false);
        client.onOpen(null);
        JsonObject payload = frame(0).getAsJsonObject("payload");
        assertFalse(payload.has("protocolVersions"));
        assertFalse(payload.has("capabilities"));
    }

    @Test
    void manifestNamesPassTakarosValidation() {
        adapter.dimensions = List.of(DIM);
        for (String name : names(Capabilities.manifest(adapter.dimensions))) {
            assertTrue(name.matches("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9-]*)*$"), name);
        }
    }

    @Test
    void mapDimensionCarriesAxesBoundsAndImageSource() {
        JsonObject dim = Capabilities.dimensionJson(DIM);
        assertEquals("knox-county", dim.get("id").getAsString());
        assertEquals("y", dim.getAsJsonObject("axes").getAsJsonObject("vertical").get("field").getAsString());
        assertEquals("down", dim.getAsJsonObject("axes").getAsJsonObject("vertical").get("direction").getAsString());
        assertEquals(19968, dim.getAsJsonObject("bounds").getAsJsonObject("horizontal").get("max").getAsDouble());
        JsonObject source = dim.getAsJsonObject("source");
        assertEquals("image", source.get("type").getAsString());
        assertEquals("rev-1", source.get("revision").getAsString());
        assertEquals(10, source.get("size").getAsLong());
        assertEquals("image/jpeg", source.get("mimeType").getAsString());
    }

    @Test
    void eventsBeforeIdentifyAreBufferedThenSentWithStreamAndSeq() {
        client.onOpen(null);
        client.emitLog("one");
        client.emitLog("two");
        assertEquals(1, sent.size(), "only identify before identifyResponse");

        identify(1);
        List<JsonObject> events = events();
        assertEquals(2, events.size());
        assertEquals("stream-1", events.get(0).getAsJsonObject("payload").get("streamId").getAsString());
        assertEquals(1, events.get(0).getAsJsonObject("payload").get("seq").getAsLong());
        assertEquals(2, events.get(1).getAsJsonObject("payload").get("seq").getAsLong());
        assertEquals("one", events.get(0).getAsJsonObject("payload").getAsJsonObject("data").get("msg").getAsString());
    }

    @Test
    void unacknowledgedEventsAreResentAfterReconnectAndAckedOnesAreNot() throws Exception {
        client.onOpen(null);
        identify(1);
        client.emitLog("a");
        client.emitLog("b");
        client.onMessage("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"stream-1\",\"upTo\":1},\"requestId\":\"x\"}");
        assertEquals(1, outbox.size());

        client.onClose(1006, "gone", true);
        client.emitLog("c"); // produced while down
        sent.clear();
        client.onOpen(null);
        identify(1);
        List<Long> seqs = new ArrayList<>();
        for (JsonObject e : events()) seqs.add(e.getAsJsonObject("payload").get("seq").getAsLong());
        assertEquals(List.of(2L, 3L), seqs);
    }

    @Test
    void outboxDropsTheOldestOnOverflow() {
        for (int i = 0; i < 5; i++) outbox.add("log", new JsonObject());
        assertEquals(3, outbox.size());
        assertEquals(2, outbox.dropped());
        assertEquals(3, outbox.unacknowledged().get(0).seq());
        assertEquals(0, outbox.acknowledge("other-stream", 99));
    }

    @Test
    void takaroPickingVersion0FlushesBufferedEventsInTheOldShape() {
        client.onOpen(null);
        client.emitLog("early");
        identify(0);
        client.emitLog("late");
        List<JsonObject> events = events();
        assertEquals(2, events.size());
        for (JsonObject e : events) {
            assertFalse(e.getAsJsonObject("payload").has("seq"));
            assertFalse(e.getAsJsonObject("payload").has("streamId"));
        }
        assertEquals(0, outbox.size());
    }

    @Test
    void missingProtocolVersionMeansVersion0() {
        client.onOpen(null);
        client.onMessage("{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs\"}}}");
        assertEquals(0, client.protocolVersion());
        assertTrue(client.isIdentified());
    }

    @Test
    void v1ErrorsUseTheCodeShape() throws Exception {
        client.onOpen(null);
        identify(1);
        sent.clear();
        request("getMapInfo", "r1", "{}");
        JsonObject response = frame(0);
        assertEquals("unsupported", response.getAsJsonObject("error").get("code").getAsString());
        assertFalse(response.has("payload"));
    }

    @Test
    void v0KeepsTheStringError() {
        client.onOpen(null);
        identify(0);
        sent.clear();
        request("getMapInfo", "r1", "{}");
        assertEquals("Action not implemented: getMapInfo", frame(0).get("error").getAsString());
    }

    @Test
    void v1VoidActionAnswersPayloadNullAndUnknownLocationIsNull() throws Exception {
        client.onOpen(null);
        identify(1);
        sent.clear();
        request("kickPlayer", "r1", "{\"gameId\":\"p\",\"reason\":\"x\"}");
        request("getPlayerLocation", "r2", "{\"gameId\":\"nobody\"}");
        Thread.sleep(200);
        for (String raw : sent) {
            JsonObject r = JsonParser.parseString(raw).getAsJsonObject();
            assertTrue(r.has("payload"), raw);
            assertTrue(r.get("payload").isJsonNull(), raw);
            assertFalse(r.has("error"), raw);
        }
        assertEquals(2, sent.size());
    }

    @Test
    void v1ActionFailureMapsToAnErrorCode() throws Exception {
        client.onOpen(null);
        identify(1);
        sent.clear();
        adapter.failGiveWith = new IllegalArgumentException("bad item");
        request("giveItem", "r1", "{\"gameId\":\"p\",\"item\":\"Base.Axe\",\"amount\":1}");
        Thread.sleep(200);
        JsonObject error = frame(0).getAsJsonObject("error");
        assertEquals("invalid_args", error.get("code").getAsString());
        assertEquals("bad item", error.get("message").getAsString());
    }

    @Test
    void getMapImageServesTheRequestedChunk() {
        adapter.dimensions = List.of(DIM);
        client.onOpen(null);
        identify(1);
        sent.clear();
        request("getMapImage", "r1", "{\"dimension\":\"knox-county\",\"offset\":4,\"length\":100}");
        JsonObject payload = frame(0).getAsJsonObject("payload");
        assertEquals("rev-1", payload.get("revision").getAsString());
        assertArrayEquals(new byte[] {4, 5, 6, 7, 8, 9}, Base64.getDecoder().decode(payload.get("data").getAsString()));
    }

    @Test
    void getMapImageRefusesBadArgsUnknownDimensionsAndOffsetsPastTheEnd() {
        adapter.dimensions = List.of(DIM);
        client.onOpen(null);
        identify(1);
        sent.clear();
        request("getMapImage", "r1", "{\"dimension\":\"knox-county\",\"offset\":0,\"length\":0}");
        request("getMapImage", "r2", "{\"dimension\":\"moon\",\"offset\":0,\"length\":5}");
        request("getMapImage", "r3", "{\"dimension\":\"knox-county\",\"offset\":10,\"length\":5}");
        request("getMapImage", "r4", "{\"dimension\":\"knox-county\",\"offset\":0,\"length\":2097152}");
        assertEquals("invalid_args", frame(0).getAsJsonObject("error").get("code").getAsString());
        assertEquals("not_found", frame(1).getAsJsonObject("error").get("code").getAsString());
        assertEquals("invalid_args", frame(2).getAsJsonObject("error").get("code").getAsString());
        assertEquals("invalid_args", frame(3).getAsJsonObject("error").get("code").getAsString());
    }

    @Test
    void getMapImageIsRefusedOnVersion0() {
        adapter.dimensions = List.of(DIM);
        client.onOpen(null);
        identify(0);
        sent.clear();
        request("getMapImage", "r1", "{\"dimension\":\"knox-county\",\"offset\":0,\"length\":5}");
        assertEquals("Action not implemented: getMapImage", frame(0).get("error").getAsString());
    }

    @Test
    void capabilitiesChangedResendsTheManifestWithTheMap() {
        client.onOpen(null);
        identify(1);
        sent.clear();
        adapter.dimensions = List.of(DIM);
        client.capabilitiesChanged();
        JsonObject msg = frame(0);
        assertEquals("capabilities", msg.get("type").getAsString());
        assertTrue(msg.has("requestId"));
        assertTrue(names(msg.getAsJsonObject("payload").getAsJsonArray("capabilities")).contains("map"));
    }

    @Test
    void capabilitiesChangedBeforeIdentifyOrOnVersion0SendsNothing() {
        client.onOpen(null);
        sent.clear();
        client.capabilitiesChanged();
        identify(0);
        client.capabilitiesChanged();
        assertTrue(sent.isEmpty());
    }

    // --- helpers ---

    private void identify(int version) {
        client.onMessage("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs\",\"protocolVersion\":"
                + version + "},\"requestId\":\"i\"}");
    }

    private void request(String action, String requestId, String args) {
        JsonObject request = new JsonObject();
        request.addProperty("type", "request");
        request.addProperty("requestId", requestId);
        JsonObject payload = new JsonObject();
        payload.addProperty("action", action);
        payload.addProperty("args", args);
        request.add("payload", payload);
        client.onMessage(request.toString());
    }

    private JsonObject frame(int i) {
        return JsonParser.parseString(sent.get(i)).getAsJsonObject();
    }

    private List<JsonObject> events() {
        List<JsonObject> out = new ArrayList<>();
        for (String raw : sent) {
            JsonObject f = JsonParser.parseString(raw).getAsJsonObject();
            if ("gameEvent".equals(f.get("type").getAsString())) out.add(f);
        }
        return out;
    }

    private static Set<String> names(JsonArray manifest) {
        Set<String> out = new HashSet<>();
        for (JsonElement e : manifest) out.add(e.getAsJsonObject().get("name").getAsString());
        return out;
    }

    private static class FakeAdapter implements GameAdapter {
        List<MapDimension> dimensions = List.of();
        RuntimeException failGiveWith;

        @Override public void logInfo(String msg) { }
        @Override public void logWarning(String msg) { }
        @Override public void runOnMainThread(Runnable task) { task.run(); }
        @Override public void setEventEmitter(EventEmitter emitter) { }
        @Override public PlayerInfo getPlayer(String gameId) { return null; }
        @Override public List<PlayerInfo> getPlayers() { return List.of(); }
        @Override public PlayerLocation getPlayerLocation(String gameId) { return null; }
        @Override public List<InventoryItem> getPlayerInventory(String gameId) { return List.of(); }
        @Override public List<GameItem> listItems() { return List.of(); }
        @Override public List<GameEntity> listEntities() { return List.of(); }
        @Override public List<GameLocation> listLocations() { return List.of(); }
        @Override public void giveItem(String gameId, String itemCode, int amount, String quality) {
            if (failGiveWith != null) throw failGiveWith;
        }
        @Override public void sendMessage(String message, String recipientGameId) { }
        @Override public CommandResult executeConsoleCommand(String command) { return new CommandResult(true, "", null); }
        @Override public void teleportPlayer(String gameId, double x, double y, double z, String dimension) { }
        @Override public void kickPlayer(String gameId, String reason) { }
        @Override public void banPlayer(String gameId, String reason, String expiresAt) { }
        @Override public void unbanPlayer(String gameId) { }
        @Override public List<BanEntry> listBans() { return List.of(); }
        @Override public void shutdownServer() { }
        @Override public List<MapDimension> getMapDimensions() { return dimensions; }

        @Override
        public MapImageChunk readMapImage(String dimension, long offset, int length) {
            if (dimensions.isEmpty() || !dimensions.get(0).id().equals(dimension)) {
                throw new ProtocolException(ProtocolException.NOT_FOUND, "no " + dimension);
            }
            if (offset >= IMAGE.length) throw new ProtocolException(ProtocolException.INVALID_ARGS, "past end");
            return new MapImageChunk("rev-1", Arrays.copyOfRange(IMAGE, (int) offset,
                    (int) Math.min(IMAGE.length, offset + length)));
        }
    }
}
