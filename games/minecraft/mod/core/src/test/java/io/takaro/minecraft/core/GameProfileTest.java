package io.takaro.minecraft.core;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.target.RuntimeIdentity;
import org.junit.jupiter.api.Test;

import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Base64;
import java.util.HexFormat;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

import static org.junit.jupiter.api.Assertions.*;

class GameProfileTest {

    // Tests run from games/minecraft/mod/core.
    private static final Path VERSION_TXT = Path.of("../../version.txt");
    private static final Path ICON_PNG = Path.of("../../assets/icon-256.png");

    private final List<String> sent = new CopyOnWriteArrayList<>();
    private final TakaroConnectorTest.TestAdapter adapter = new TakaroConnectorTest.TestAdapter();

    private TakaroWebSocketClient client() throws Exception {
        TakaroConfig config = new TakaroConfig();
        config.setIdentityToken("id");
        config.setRegistrationToken("reg");
        return new TakaroWebSocketClient(new URI("ws://localhost:9999"), adapter, config, "config",
                new EventOutbox(), null) {
            @Override
            public void send(String text) { sent.add(text); }

            @Override
            public boolean isOpen() { return true; }
        };
    }

    private JsonObject identify(TakaroWebSocketClient client) {
        client.onOpen(null);
        return sent.stream().map(s -> JsonParser.parseString(s).getAsJsonObject())
                .filter(m -> m.get("type").getAsString().equals("identify")).findFirst().orElseThrow()
                .getAsJsonObject("payload");
    }

    private static JsonObject profile(JsonObject identify) {
        return identify.getAsJsonArray("capabilities").asList().stream().map(JsonElement::getAsJsonObject)
                .filter(c -> c.get("name").getAsString().equals("game.profile")).findFirst().orElseThrow()
                .getAsJsonObject("parameters");
    }

    @Test
    void identifyNamesTheGameAndTheConnectorAtTheReleaseVersion() throws Exception {
        JsonObject identify = identify(client());

        assertEquals("minecraft", identify.get("game").getAsString());
        JsonObject connector = identify.getAsJsonObject("connector");
        assertEquals("takaro-minecraft", connector.get("name").getAsString());
        assertEquals(Files.readString(VERSION_TXT).trim(), connector.get("version").getAsString());
        assertEquals("[0,1]", identify.get("protocolVersions").toString());
    }

    @Test
    void theReportedVersionIsVersionTxt() throws Exception {
        String expected = Files.readString(VERSION_TXT).trim();
        assertEquals(expected, GameProfile.connectorVersion());
        assertTrue(GameProfile.isSemver(expected), expected);
    }

    @Test
    void manifestDeclaresTheProfileWithTheRunningBuildAndTheShippedIcon() throws Exception {
        adapter.identity = new RuntimeIdentity("26.2", "fabric", "0.19.5", 25);
        JsonObject profile = profile(identify(client()));

        assertEquals("Minecraft", profile.get("name").getAsString());
        assertEquals("26.2", profile.get("version").getAsString());
        byte[] png = Files.readAllBytes(ICON_PNG);
        JsonObject icon = profile.getAsJsonObject("icon");
        assertEquals(png.length, icon.get("size").getAsInt());
        String sha = HexFormat.of().formatHex(java.security.MessageDigest.getInstance("SHA-256").digest(png));
        assertEquals(sha.substring(0, 12), icon.get("revision").getAsString());
    }

    @Test
    void withoutARuntimeBuildTheVersionIsOmitted() throws Exception {
        adapter.identity = null;
        JsonObject profile = profile(identify(client()));
        assertFalse(profile.has("version"));
        assertTrue(profile.has("icon"));
    }

    @Test
    void getGameIconServesTheWholeDeclaredPng() throws Exception {
        TakaroWebSocketClient client = client();
        JsonObject declared = profile(identify(client)).getAsJsonObject("icon");
        client.onMessage("{\"type\":\"identifyResponse\",\"payload\":{\"protocolVersion\":1}}");

        client.onMessage("{\"type\":\"request\",\"requestId\":\"icon-1\",\"payload\":{\"action\":\"getGameIcon\",\"args\":\"{}\"}}");
        JsonObject response = sent.stream().map(s -> JsonParser.parseString(s).getAsJsonObject())
                .filter(m -> m.get("type").getAsString().equals("response")).findFirst().orElseThrow();
        assertEquals("icon-1", response.get("requestId").getAsString());
        JsonObject payload = response.getAsJsonObject("payload");
        assertEquals(declared.get("revision").getAsString(), payload.get("revision").getAsString());

        byte[] data = Base64.getDecoder().decode(payload.get("data").getAsString());
        assertEquals(declared.get("size").getAsInt(), data.length);
        assertArrayEquals(Files.readAllBytes(ICON_PNG), data);
    }

    @Test
    void theShippedIconIsA256SquarePngWithinTheLimit() throws Exception {
        byte[] png = Files.readAllBytes(ICON_PNG);
        assertTrue(png.length <= 131072, "icon is " + png.length + " bytes");
        byte[] signature = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        assertArrayEquals(signature, Arrays.copyOf(png, 8));
        assertEquals("IHDR", new String(png, 12, 4, StandardCharsets.US_ASCII));
        ByteBuffer ihdr = ByteBuffer.wrap(png, 16, 8);
        assertEquals(256, ihdr.getInt());
        assertEquals(256, ihdr.getInt());
    }

    @Test
    void gameVersionKeepsOnlyWhatTakaroAccepts() {
        assertEquals("1.21.11", GameProfile.gameVersion(" 1.21.11 "));
        assertNull(GameProfile.gameVersion(""));
        assertNull(GameProfile.gameVersion("x".repeat(33)));
        assertNull(GameProfile.gameVersion("26.2\n"  + "x"));
        assertNull(GameProfile.gameVersion("26.2é"));
        assertNull(GameProfile.gameVersion(null));
    }

    @Test
    void connectorVersionMustBeStrictSemver() {
        assertTrue(GameProfile.isSemver("0.5.0"));
        assertTrue(GameProfile.isSemver("0.6.0-dev.12+abc1234"));
        assertFalse(GameProfile.isSemver("0.5"));
        assertFalse(GameProfile.isSemver("v0.5.0"));
        assertFalse(GameProfile.isSemver("01.5.0"));
    }
}
