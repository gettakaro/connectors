package io.takaro.minecraft.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.map.TestRegions;
import io.takaro.minecraft.core.map.WorldMap;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.net.URI;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.stream.Collectors;

import static org.junit.jupiter.api.Assertions.*;

class ProtocolV1Test {

    @TempDir
    Path dir;

    private final List<String> sent = new CopyOnWriteArrayList<>();
    private volatile boolean open = true;
    private WorldMap worldMap;

    @AfterEach
    void tearDown() {
        if (worldMap != null) worldMap.close();
    }

    private TakaroWebSocketClient client(boolean mapEnabled, EventOutbox outbox) throws Exception {
        TakaroConfig config = new TakaroConfig();
        config.setMapEnabled(mapEnabled);
        config.setIdentityToken("id");
        config.setRegistrationToken("reg");
        if (mapEnabled) {
            Map<Integer, Map<String, Object>> chunks = new HashMap<>();
            chunks.put(0, TestRegions.chunk(0, 0, "minecraft:grass_block", (x, z) -> 64, false));
            TestRegions.writeRegion(dir.resolve("region/r.0.0.mca"), chunks);
            worldMap = new WorldMap(List.of(new WorldMap.Dimension("overworld", "Overworld", dir.resolve("region"), false)),
                    TestRegions.colors(), s -> {});
        }
        return new TakaroWebSocketClient(new URI("ws://localhost:9999"), new TakaroConnectorTest.TestAdapter(),
                config, "config", outbox, worldMap) {
            @Override
            public void send(String text) { sent.add(text); }

            @Override
            public boolean isOpen() { return open; }
        };
    }

    private static JsonObject json(String s) {
        return JsonParser.parseString(s).getAsJsonObject();
    }

    private List<JsonObject> sentOfType(String type) {
        return sent.stream().map(ProtocolV1Test::json).filter(m -> type.equals(m.get("type").getAsString()))
                .collect(Collectors.toList());
    }

    private static String identifyResponse(int protocol) {
        return "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-1\",\"protocolVersion\":" + protocol + "}}";
    }

    private static String request(String id, String action, String args) {
        JsonObject payload = new JsonObject();
        payload.addProperty("action", action);
        payload.addProperty("args", args);
        JsonObject msg = new JsonObject();
        msg.addProperty("type", "request");
        msg.addProperty("requestId", id);
        msg.add("payload", payload);
        return msg.toString();
    }

    private JsonObject awaitResponse(String requestId) throws InterruptedException {
        for (int i = 0; i < 200; i++) {
            for (String s : sent) {
                JsonObject m = json(s);
                if ("response".equals(m.get("type").getAsString()) && requestId.equals(m.get("requestId").getAsString())) {
                    return m;
                }
            }
            Thread.sleep(25);
        }
        fail("no response to " + requestId);
        return null;
    }

    @Test
    void identifyOffersV1WithTheCapabilityManifestAndTheMap() throws Exception {
        TakaroWebSocketClient client = client(true, new EventOutbox());
        client.onOpen(null);

        JsonObject identify = sentOfType("identify").get(0).getAsJsonObject("payload");
        assertEquals("[0,1]", identify.get("protocolVersions").toString());
        JsonArray caps = identify.getAsJsonArray("capabilities");
        List<String> names = new ArrayList<>();
        caps.forEach(c -> names.add(c.getAsJsonObject().get("name").getAsString()));
        for (String action : TakaroWebSocketClient.ACTION_CAPABILITIES) assertTrue(names.contains(action), action);
        assertTrue(names.contains("events.chat-message"));
        JsonObject map = caps.asList().stream().map(JsonElement::getAsJsonObject)
                .filter(c -> c.get("name").getAsString().equals("map")).findFirst().orElseThrow();
        JsonObject dim = map.getAsJsonObject("parameters").getAsJsonArray("dimensions").get(0).getAsJsonObject();
        assertEquals("overworld", dim.get("id").getAsString());
        assertEquals("down", dim.getAsJsonObject("axes").getAsJsonObject("vertical").get("direction").getAsString());
        assertEquals(-512, dim.getAsJsonObject("bounds").getAsJsonObject("horizontal").get("min").getAsInt());
        assertEquals("tiled", dim.getAsJsonObject("source").get("type").getAsString());
    }

    @Test
    void withoutTheMapSettingIdentifyStaysV0() throws Exception {
        TakaroWebSocketClient client = client(false, new EventOutbox());
        client.onOpen(null);

        JsonObject identify = sentOfType("identify").get(0).getAsJsonObject("payload");
        assertFalse(identify.has("protocolVersions"));
        assertFalse(identify.has("capabilities"));
    }

    @Test
    void eventsCarryStreamAndSeqAndAreResentUntilAcked() throws Exception {
        EventOutbox outbox = new EventOutbox("stream-a");
        TakaroWebSocketClient client = client(true, outbox);
        client.onOpen(null);

        // Raised before identify: held, not sent.
        client.emitChatMessage("u1", "Steve", "global", "early");
        assertTrue(sentOfType("gameEvent").isEmpty());

        client.onMessage(identifyResponse(1));
        client.emitChatMessage("u1", "Steve", "global", "late");
        List<JsonObject> events = sentOfType("gameEvent");
        assertEquals(2, events.size());
        JsonObject first = events.get(0).getAsJsonObject("payload");
        assertEquals("stream-a", first.get("streamId").getAsString());
        assertEquals(1, first.get("seq").getAsLong());
        assertEquals("early", first.getAsJsonObject("data").get("msg").getAsString());
        assertEquals(2, events.get(1).getAsJsonObject("payload").get("seq").getAsLong());

        client.onMessage("{\"type\":\"eventAck\",\"requestId\":\"r\",\"payload\":{\"streamId\":\"stream-a\",\"upTo\":1}}");
        assertEquals(1, outbox.size());

        // Reconnect: only the unacked event goes again, with its original seq.
        sent.clear();
        client.onOpen(null);
        client.onMessage(identifyResponse(1));
        List<JsonObject> resent = sentOfType("gameEvent");
        assertEquals(1, resent.size());
        assertEquals(2, resent.get(0).getAsJsonObject("payload").get("seq").getAsLong());
    }

    @Test
    void whenTakaroPicksV0EventsStayPlain() throws Exception {
        EventOutbox outbox = new EventOutbox("stream-b");
        TakaroWebSocketClient client = client(true, outbox);
        client.onOpen(null);
        client.onMessage(identifyResponse(0));

        client.emitChatMessage("u1", "Steve", "global", "hi");
        JsonObject payload = sentOfType("gameEvent").get(0).getAsJsonObject("payload");
        assertFalse(payload.has("seq"));
        assertFalse(payload.has("streamId"));
        assertEquals(0, outbox.size());
    }

    @Test
    void servesMapTilesAsBase64Png() throws Exception {
        TakaroWebSocketClient client = client(true, new EventOutbox());
        client.onOpen(null);
        client.onMessage(identifyResponse(1));

        client.onMessage(request("t1", "getMapTile", "{\"dimension\":\"overworld\",\"zoom\":0,\"x\":0,\"y\":0}"));
        JsonObject tile = awaitResponse("t1").getAsJsonObject("payload");
        assertEquals("image/png", tile.get("mimeType").getAsString());
        byte[] png = Base64.getDecoder().decode(tile.get("data").getAsString());
        assertEquals((byte) 0x89, png[0]);
        assertEquals('P', png[1]);
    }

    @Test
    void v1ErrorsCarryProtocolCodes() throws Exception {
        TakaroWebSocketClient client = client(true, new EventOutbox());
        client.onOpen(null);
        client.onMessage(identifyResponse(1));

        client.onMessage(request("e1", "getMapTile", "{\"dimension\":\"overworld\",\"zoom\":9,\"x\":0,\"y\":0}"));
        assertEquals("invalid_args", awaitResponse("e1").getAsJsonObject("error").get("code").getAsString());

        client.onMessage(request("e2", "getMapTile", "{\"x\":0,\"y\":0,\"z\":0}"));
        assertEquals("invalid_args", awaitResponse("e2").getAsJsonObject("error").get("code").getAsString());

        client.onMessage(request("e3", "getMapImage", "{}"));
        assertEquals("unsupported", awaitResponse("e3").getAsJsonObject("error").get("code").getAsString());

        // An unknown player is a null payload on v1, not {}.
        client.onMessage(request("e4", "getPlayerLocation", "{\"gameId\":\"nobody\"}"));
        JsonObject r = awaitResponse("e4");
        assertTrue(r.has("payload"));
        assertTrue(r.get("payload").isJsonNull());
        assertFalse(r.has("error"));
    }

    @Test
    void aV1IdentifyErrorIsNotTakenForSuccess() throws Exception {
        TakaroConnectorTest.TestAdapter adapter = new TakaroConnectorTest.TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setMapEnabled(true);
        TakaroWebSocketClient client = new TakaroWebSocketClient(new URI("ws://localhost:9999"), adapter, config) {
            @Override
            public void send(String text) { sent.add(text); }

            @Override
            public boolean isOpen() { return false; }
        };
        client.onMessage("{\"type\":\"identifyResponse\",\"payload\":null,"
                + "\"error\":{\"code\":\"no_shared_protocol_version\",\"message\":\"no shared protocol version\"}}");
        assertTrue(adapter.infos.stream().noneMatch(s -> s.startsWith("Identified")));
        assertTrue(adapter.warnings.stream().anyMatch(s -> s.contains("no shared protocol version")));
        assertFalse(client.isV1());
    }

    @Test
    void aSilentOpenConnectionIsDroppedSoItReconnects() throws Exception {
        java.util.concurrent.atomic.AtomicInteger drops = new java.util.concurrent.atomic.AtomicInteger();
        TakaroConfig config = new TakaroConfig();
        TakaroWebSocketClient client = new TakaroWebSocketClient(new URI("ws://localhost:9999"),
                new TakaroConnectorTest.TestAdapter(), config, "config", new EventOutbox(), null) {
            @Override
            public void send(String text) { sent.add(text); }

            @Override
            public boolean isOpen() { return open; }

            @Override
            void dropConnection() { drops.incrementAndGet(); }
        };
        client.setSilenceLimitMillis(200);
        client.onMessage(identifyResponse(0));

        // Takaro keeps talking: no drop.
        client.onMessage("{\"type\":\"connected\",\"payload\":{}}");
        client.checkSilence();
        assertEquals(0, drops.get());

        Thread.sleep(300);
        client.checkSilence();
        assertEquals(1, drops.get());
        // Not identified any more: a second tick does not drop again.
        client.checkSilence();
        assertEquals(1, drops.get());
    }
}
