package io.takaro.zomboid.core;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

/** Core fix (b): tolerant args parsing and gameId extraction across shapes. */
class ArgShapeTest {

    private static JsonObject args(String json) {
        return TakaroWebSocketClient.parseArgs(JsonParser.parseString(json));
    }

    @Test
    void parseArgsFromJsonStringObject() {
        JsonObject a = TakaroWebSocketClient.parseArgs(
                JsonParser.parseString("\"{\\\"command\\\":\\\"players\\\"}\""));
        assertEquals("players", a.get("command").getAsString());
    }

    @Test
    void parseArgsFromJsonObject() {
        assertEquals("players", args("{\"command\":\"players\"}").get("command").getAsString());
    }

    @Test
    void parseArgsFromEmptyObjectStringAndArray() {
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("\"{}\"")).size());
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("\"[]\"")).size());
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("{}")).size());
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("[]")).size());
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("\"\"")).size());
    }

    @Test
    void parseArgsFromNullIsEmptyObject() {
        assertEquals(0, TakaroWebSocketClient.parseArgs(null).size());
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("null")).size());
    }

    @Test
    void parseArgsFromGarbageIsEmptyObject() {
        assertEquals(0, TakaroWebSocketClient.parseArgs(JsonParser.parseString("\"not json\"")).size());
    }

    @Test
    void extractGameIdFlat() {
        assertEquals("p1", TakaroWebSocketClient.extractGameId(args("{\"gameId\":\"p1\"}")));
    }

    @Test
    void extractGameIdNestedPlayer() {
        assertEquals("p2", TakaroWebSocketClient.extractGameId(args("{\"player\":{\"gameId\":\"p2\"}}")));
    }

    @Test
    void extractGameIdNestedPlayerRef() {
        assertEquals("p3", TakaroWebSocketClient.extractGameId(args("{\"playerRef\":{\"gameId\":\"p3\"}}")));
    }

    @Test
    void extractGameIdFlatWinsOverNested() {
        assertEquals("flat",
                TakaroWebSocketClient.extractGameId(args("{\"gameId\":\"flat\",\"player\":{\"gameId\":\"nested\"}}")));
    }

    @Test
    void extractGameIdMissingIsNull() {
        assertNull(TakaroWebSocketClient.extractGameId(args("{}")));
        assertNull(TakaroWebSocketClient.extractGameId(args("{\"player\":{}}")));
    }
}
