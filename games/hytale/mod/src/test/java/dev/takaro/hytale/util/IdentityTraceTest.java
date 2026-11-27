package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class IdentityTraceTest {

    private static Map<String, Object> player(String uuid) {
        Map<String, Object> p = new LinkedHashMap<>();
        p.put("name", "Someone");
        p.put("gameId", uuid);
        p.put("platformId", "hytale:" + uuid);
        return p;
    }

    @Test
    void gameEventPlayerAndAttackerAreTraced() {
        Map<String, Object> data = new LinkedHashMap<>();
        data.put("msg", "secret chat text");
        data.put("player", player("a"));
        data.put("attacker", player("b"));
        Map<String, Object> payload = new HashMap<>();
        payload.put("type", "player-death");
        payload.put("data", data);
        Map<String, Object> frame = new HashMap<>();
        frame.put("type", "gameEvent");
        frame.put("payload", payload);

        String trace = IdentityTrace.of(frame);
        assertEquals("[gameId=a platformId=hytale:a] [gameId=b platformId=hytale:b]", trace);
        assertFalse(trace.contains("secret"));
    }

    @Test
    void getPlayersResponseListIsTraced() {
        Map<String, Object> frame = new HashMap<>();
        frame.put("type", "response");
        frame.put("payload", List.of(player("a"), player("b")));
        assertEquals("[gameId=a platformId=hytale:a] [gameId=b platformId=hytale:b]", IdentityTrace.of(frame));
    }

    @Test
    void steamIdWouldBeVisible() {
        Map<String, Object> p = new LinkedHashMap<>();
        p.put("gameId", "a");
        p.put("steamId", "a");
        assertEquals("[gameId=a steamId=a]", IdentityTrace.of(Map.of("payload", p)));
    }

    @Test
    void framesWithoutPlayersGiveNull() {
        assertNull(IdentityTrace.of(Map.of("type", "pong")));
        assertNull(IdentityTrace.of(Map.of("payload", Map.of("gameId", "x"))));
        assertNull(IdentityTrace.of(null));
    }
}
