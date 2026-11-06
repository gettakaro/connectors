package io.takaro.minecraft.core;

import org.apache.logging.log4j.LogManager;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

import static org.junit.jupiter.api.Assertions.*;

class ServerLogForwarderTest {

    private final List<String> lines = new CopyOnWriteArrayList<>();
    private ServerLogForwarder forwarder;

    @BeforeEach
    void attach() {
        forwarder = ServerLogForwarder.attach(lines::add);
    }

    @AfterEach
    void detach() {
        forwarder.detach();
    }

    @Test
    void forwardsServerLines() {
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("Steve joined the game");
        assertEquals(List.of("Steve joined the game"), lines);
    }

    @Test
    void skipsTheConnectorsOwnLinesSoASendNeverLogsAnother() {
        LogManager.getLogger("Takaro").error("WS SEND gameEvent");
        LogManager.getLogger("TakaroMinecraft").error("WS SEND gameEvent");
        LogManager.getLogger("org.java_websocket.WebSocketImpl").error("write");
        assertTrue(lines.isEmpty());
    }

    @Test
    void stopsForwardingOnceDetached() {
        forwarder.detach();
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("after");
        forwarder = ServerLogForwarder.attach(lines::add);
        assertTrue(lines.isEmpty());
    }

    @Test
    void limitsLinesPerWindowAndRecoversInTheNext() {
        long start = 1_000_000;
        for (int i = 0; i < ServerLogForwarder.MAX_LINES_PER_WINDOW; i++) {
            assertTrue(forwarder.allow(start));
        }
        assertFalse(forwarder.allow(start + 1));
        assertTrue(forwarder.allow(start + ServerLogForwarder.WINDOW_MILLIS));
    }
}
