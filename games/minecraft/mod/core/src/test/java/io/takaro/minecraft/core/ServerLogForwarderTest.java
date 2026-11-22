package io.takaro.minecraft.core;

import org.apache.logging.log4j.LogManager;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BooleanSupplier;

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

    private static void await(BooleanSupplier done) throws InterruptedException {
        for (int i = 0; i < 200 && !done.getAsBoolean(); i++) {
            Thread.sleep(10);
        }
    }

    @Test
    void forwardsServerLines() throws InterruptedException {
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("Steve joined the game");
        await(() -> !lines.isEmpty());
        assertEquals(List.of("Steve joined the game"), lines);
    }

    @Test
    void sendsOffTheLoggingThread() throws InterruptedException {
        List<String> threads = new CopyOnWriteArrayList<>();
        forwarder.detach();
        forwarder = ServerLogForwarder.attach(line -> threads.add(Thread.currentThread().getName()));
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("on the server thread");
        await(() -> !threads.isEmpty());
        assertEquals(List.of("takaro-log-forward"), threads);
    }

    @Test
    void aSendThatLogsLosesNoLine() throws InterruptedException {
        // A send that itself logs a server line used to run inside log4j's append, which refuses the
        // inner call as recursive and drops that line.
        forwarder.detach();
        forwarder = ServerLogForwarder.attach(line -> {
            lines.add(line);
            if (line.equals("first")) {
                LogManager.getLogger("net.minecraft.server.rcon").error("logged during the send");
            }
        });
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("first");
        await(() -> lines.size() >= 2);
        assertEquals(List.of("first", "logged during the send"), lines);
    }

    @Test
    void skipsTheConnectorsOwnLinesSoASendNeverLogsAnother() throws InterruptedException {
        LogManager.getLogger("Takaro").error("WS SEND gameEvent");
        LogManager.getLogger("TakaroMinecraft").error("WS SEND gameEvent");
        LogManager.getLogger("org.java_websocket.WebSocketImpl").error("write");
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("marker");
        await(() -> !lines.isEmpty());
        assertEquals(List.of("marker"), lines);
    }

    @Test
    void stopsForwardingOnceDetached() throws InterruptedException {
        forwarder.detach();
        LogManager.getLogger("net.minecraft.server.MinecraftServer").error("after");
        Thread.sleep(100);
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
