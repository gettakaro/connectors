package io.takaro.zomboid.core;

import io.takaro.zomboid.core.model.*;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class TakaroConnectorTest {

    private static class TestAdapter implements GameAdapter {
        final List<String> infos = new ArrayList<>();
        final List<String> warnings = new ArrayList<>();
        final List<String> debugs = new ArrayList<>();

        @Override
        public void logInfo(String msg) { infos.add(msg); }

        @Override
        public void logWarning(String msg) { warnings.add(msg); }

        @Override
        public void logDebug(String msg) { debugs.add(msg); }

        @Override
        public void runOnMainThread(Runnable task) { task.run(); }

        @Override
        public PlayerInfo getPlayer(String gameId) { return null; }

        @Override
        public List<PlayerInfo> getPlayers() { return Collections.emptyList(); }

        @Override
        public PlayerLocation getPlayerLocation(String gameId) { return null; }

        @Override
        public List<InventoryItem> getPlayerInventory(String gameId) { return Collections.emptyList(); }

        @Override
        public List<GameItem> listItems() { return Collections.emptyList(); }

        @Override
        public List<GameEntity> listEntities() { return Collections.emptyList(); }

        @Override
        public List<GameLocation> listLocations() { return Collections.emptyList(); }

        @Override
        public void giveItem(String gameId, String itemCode, int amount, String quality) {}

        @Override
        public void sendMessage(String message, String recipientGameId) {}

        @Override
        public CommandResult executeConsoleCommand(String command) {
            return new CommandResult(true, "", null);
        }

        @Override
        public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {}

        @Override
        public void kickPlayer(String gameId, String reason) {}

        @Override
        public void banPlayer(String gameId, String reason, String expiresAt) {}

        @Override
        public void unbanPlayer(String gameId) {}

        @Override
        public List<BanEntry> listBans() { return Collections.emptyList(); }

        @Override
        public void shutdownServer() {}

        @Override
        public void setEventEmitter(EventEmitter emitter) {}
    }

    @Test
    void connectWithNullUrlLogsWarning() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();

        TakaroConnector connector = new TakaroConnector(adapter, config);
        connector.connect();

        assertEquals(1, adapter.warnings.size());
        assertTrue(adapter.warnings.get(0).contains("No WebSocket URL configured"));
        assertTrue(adapter.infos.isEmpty());
    }

    @Test
    void connectWithEmptyUrlLogsWarning() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("");

        TakaroConnector connector = new TakaroConnector(adapter, config);
        connector.connect();

        assertEquals(1, adapter.warnings.size());
        assertTrue(adapter.warnings.get(0).contains("No WebSocket URL configured"));
    }

    @Test
    void connectWithInvalidUrlLogsError() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("not a valid url %%");
        config.setRegistrationToken("reg");

        TakaroConnector connector = new TakaroConnector(adapter, config);
        connector.connect();

        assertEquals(1, adapter.infos.size());
        assertTrue(adapter.infos.get(0).contains("Connecting to Takaro"));
        assertEquals(1, adapter.warnings.size());
        assertTrue(adapter.warnings.get(0).contains("Failed to create WebSocket connection"));
    }

    @Test
    void missingRegistrationTokenLogsTheBannerAndDoesNotConnect() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:1/");
        config.setConfigFileHint("/srv/Zomboid/Takaro/TakaroConfig.txt");

        new TakaroConnector(adapter, config).connect();

        assertTrue(adapter.infos.stream().noneMatch(i -> i.contains("Connecting to Takaro")));
        String banner = String.join("\n", adapter.warnings);
        assertTrue(banner.contains("registrationToken not set, the server is not connected to Takaro."));
        assertTrue(banner.contains("/srv/Zomboid/Takaro/TakaroConfig.txt"));
        assertTrue(banner.contains("no restart needed"));
        assertTrue(adapter.warnings.get(0).startsWith("*****"));
    }

    @Test
    void configChangeBeforeTheFirstTickOnlyUpdatesTheSettings() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        TakaroConnector connector = new TakaroConnector(adapter, config);
        TakaroConfig updated = new TakaroConfig();
        updated.setWsUrl("ws://127.0.0.1:1/");
        updated.setRegistrationToken("reg");

        connector.applyConfig(updated);

        assertEquals("reg", config.getRegistrationToken());
        assertTrue(adapter.infos.isEmpty(), "nothing connects before connect()");
    }

    @Test
    void configChangeAfterShutdownDoesNothing() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:1/");
        TakaroConnector connector = new TakaroConnector(adapter, config);
        connector.connect();
        connector.shutdown();
        adapter.infos.clear();
        TakaroConfig updated = new TakaroConfig();
        updated.setWsUrl("ws://127.0.0.1:1/");
        updated.setRegistrationToken("reg");

        connector.applyConfig(updated);
        connector.connect();

        assertTrue(adapter.infos.isEmpty());
    }

    @Test
    void shutdownWithNoConnectionDoesNotThrow() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();

        TakaroConnector connector = new TakaroConnector(adapter, config);
        assertDoesNotThrow(connector::shutdown);
    }
}
