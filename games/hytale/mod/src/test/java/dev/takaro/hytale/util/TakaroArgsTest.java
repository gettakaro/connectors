package dev.takaro.hytale.util;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class TakaroArgsTest {
    private static final Gson GSON = new Gson();

    /** Build the {"args": "<json string>"} envelope Takaro actually sends. */
    private static TakaroArgs args(String json) {
        JsonObject payload = new JsonObject();
        payload.addProperty("args", json);
        return TakaroArgs.of(payload);
    }

    @Test
    void explicitJsonNullIsTreatedAsAbsentForEveryType() {
        TakaroArgs a = args("{\"amount\":null,\"reason\":null,\"opts\":null,\"x\":null}");
        assertEquals(1, a.intVal("amount", 1));
        assertEquals("You were kicked.", a.nonBlank("reason", "You were kicked."));
        assertNull(a.obj("opts"));
        assertNull(a.recipientGameId());
        assertFalse(a.hasNumber("x"));
        assertFalse(a.has("amount"));
    }

    @Test
    void missingKeysFallBackToDefaults() {
        TakaroArgs a = args("{}");
        assertEquals(7, a.intVal("amount", 7));
        assertEquals(1.5, a.dblVal("x", 1.5));
        assertEquals("fallback", a.str("item", "fallback"));
        assertNull(a.playerGameId());
    }

    @Test
    void wrongTypesDoNotThrow() {
        TakaroArgs a = args("{\"amount\":\"12\",\"x\":\"3.5\",\"item\":{\"nested\":true},\"reason\":42}");
        assertEquals(12, a.intVal("amount", 1));
        assertEquals(3.5, a.dblVal("x", 0));
        assertEquals("fallback", a.str("item", "fallback"));  // object is not a usable string
        assertEquals("42", a.str("reason", null));            // number renders as a string
    }

    @Test
    void emptyReasonFallsBackToTheDefault() {
        assertEquals("default", args("{\"reason\":\"\"}").nonBlank("reason", "default"));
        assertEquals("default", args("{\"reason\":\"   \"}").nonBlank("reason", "default"));
        assertEquals("rude", args("{\"reason\":\"rude\"}").nonBlank("reason", "default"));
    }

    @Test
    void playerIdResolvesFromNestedAndFlatForms() {
        String id = "00000000-0000-4000-8000-000000000001";
        assertEquals(id, args("{\"player\":{\"gameId\":\"" + id + "\"}}").playerGameId());
        assertEquals(id, args("{\"gameId\":\"" + id + "\"}").playerGameId());
        assertEquals(id, args("{\"playerId\":\"" + id + "\"}").playerGameId());

        // top level of the payload, with no args at all
        JsonObject payload = new JsonObject();
        payload.addProperty("gameId", id);
        assertEquals(id, TakaroArgs.of(payload).playerGameId());
    }

    @Test
    void nestedPlayerObjectWithNullGameIdDoesNotThrow() {
        assertNull(args("{\"player\":{\"gameId\":null}}").playerGameId());
        assertNull(args("{\"player\":null}").playerGameId());
    }

    @Test
    void recipientIsReadThroughOptsWithoutThrowing() {
        String id = "aaaa0000-0000-0000-0000-000000000001";
        assertEquals(id, args("{\"message\":\"hi\",\"opts\":{\"recipient\":{\"gameId\":\"" + id + "\"}}}")
            .recipientGameId());
        assertNull(args("{\"message\":\"hi\",\"opts\":{\"recipient\":null}}").recipientGameId());
        assertNull(args("{\"message\":\"hi\"}").recipientGameId());
    }

    @Test
    void argsMayAlsoArriveAsARealJsonObject() {
        JsonObject inner = GSON.fromJson("{\"item\":\"Wood_Oak_Trunk\",\"amount\":5}", JsonObject.class);
        JsonObject payload = new JsonObject();
        payload.add("args", inner);
        TakaroArgs a = TakaroArgs.of(payload);
        assertEquals("Wood_Oak_Trunk", a.str("item"));
        assertEquals(5, a.intVal("amount", 1));
    }

    @Test
    void malformedArgsStringDoesNotThrow() {
        TakaroArgs a = args("not json at all");
        assertNull(a.str("item"));
        assertEquals(1, a.intVal("amount", 1));
    }

    @Test
    void nullPayloadIsSafe() {
        TakaroArgs a = TakaroArgs.of(null);
        assertNull(a.playerGameId());
        assertEquals(3, a.intVal("amount", 3));
    }

    @Test
    void targetGameIdSupportsBothSpellings() {
        String id = "bbbb0000-0000-0000-0000-000000000002";
        assertEquals(id, args("{\"targetGameId\":\"" + id + "\"}").targetGameId());
        assertEquals(id, args("{\"targetPlayerId\":\"" + id + "\"}").targetGameId());
        assertEquals(id, args("{\"target\":{\"gameId\":\"" + id + "\"}}").targetGameId());
        assertNull(args("{\"targetGameId\":null}").targetGameId());
    }
}
