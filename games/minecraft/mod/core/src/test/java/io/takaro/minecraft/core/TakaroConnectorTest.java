package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.*;
import io.takaro.minecraft.core.target.RuntimeIdentity;
import io.takaro.minecraft.core.target.TargetInfo;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Optional;

import static org.junit.jupiter.api.Assertions.*;

class TakaroConnectorTest {

    private static final String TARGET_CHECK = "Takaro target-check: ";

    static class TestAdapter implements GameAdapter {
        final List<String> infos = new ArrayList<>();
        final List<String> warnings = new ArrayList<>();
        final List<String> debugs = new ArrayList<>();
        RuntimeIdentity identity;

        @Override
        public RuntimeIdentity getRuntimeIdentity() { return identity; }

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
        config.setIdentityToken("id");

        TakaroConnector connector = new TakaroConnector(adapter, config);
        connector.connect();

        assertTrue(adapter.infos.stream().anyMatch(line -> line.contains("Connecting to Takaro")));
        assertEquals(1, adapter.warnings.size());
        assertTrue(adapter.warnings.get(0).contains("Failed to create WebSocket connection"));
    }

    @Test
    void shutdownWithNoConnectionDoesNotThrow() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();

        TakaroConnector connector = new TakaroConnector(adapter, config);
        assertDoesNotThrow(connector::shutdown);
    }

    private static TargetInfo fabric262() {
        return TargetInfo.of("fabric-26.2", "f".repeat(64), "minecraft", "fabric", "26.2",
                "0.1.1", "abc123", "26.2", "0.19.5", 25);
    }

    private static String targetCheckLine(TestAdapter adapter) {
        return adapter.infos.stream()
                .filter(line -> line.startsWith(TARGET_CHECK))
                .findFirst()
                .orElseThrow(() -> new AssertionError("no target-check line was logged"));
    }

    @Test
    void refuseDoesNotConnect() {
        TestAdapter adapter = new TestAdapter();
        adapter.identity = new RuntimeIdentity("26.1.2", "fabric", "0.19.5", 25);
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:1/");
        config.setRegistrationToken("reg");
        config.setIdentityToken("id");

        new TakaroConnector(adapter, config, Optional.of(fabric262())).connect();

        String line = targetCheckLine(adapter);
        assertTrue(line.contains("\"result\":\"refuse\""), line);
        assertEquals(1, adapter.warnings.size());
        assertTrue(adapter.warnings.get(0).contains("Takaro refuses to connect"));
        assertTrue(adapter.warnings.get(0).contains("TAKARO_TARGET_POLICY=warn"));
        assertTrue(adapter.infos.stream().noneMatch(l -> l.contains("Connecting to Takaro")),
                "a refused target must not open a connection");
    }

    @Test
    void warnPolicyStillConnects() {
        TestAdapter adapter = new TestAdapter();
        adapter.identity = new RuntimeIdentity("26.1.2", "fabric", "0.19.5", 25);
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("not a valid url %%");
        config.setRegistrationToken("reg");
        config.setIdentityToken("id");
        config.setTargetPolicy("warn");

        new TakaroConnector(adapter, config, Optional.of(fabric262())).connect();

        String line = targetCheckLine(adapter);
        assertTrue(line.contains("\"result\":\"refuse\""), line);
        assertTrue(line.contains("\"policy\":\"warn\""), line);
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("Connecting to Takaro")),
                "warn policy still connects");
    }

    @Test
    void matchingTargetLogsOkAndConnects() {
        TestAdapter adapter = new TestAdapter();
        adapter.identity = new RuntimeIdentity("26.2", "fabric", "0.19.5", 25);
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("not a valid url %%");
        config.setRegistrationToken("reg");
        config.setIdentityToken("id");

        new TakaroConnector(adapter, config, Optional.of(fabric262())).connect();

        String line = targetCheckLine(adapter);
        assertTrue(line.contains("\"result\":\"ok\""), line);
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("Connecting to Takaro")));
    }

    @Test
    void unstampedBuildIsUncheckedAndConnects() {
        TestAdapter adapter = new TestAdapter();
        adapter.identity = new RuntimeIdentity("26.2", "fabric", "0.19.5", 25);
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("not a valid url %%");
        config.setRegistrationToken("reg");
        config.setIdentityToken("id");

        new TakaroConnector(adapter, config, Optional.empty()).connect();

        assertTrue(targetCheckLine(adapter).contains("\"result\":\"unchecked\""));
        assertTrue(adapter.infos.stream().anyMatch(l -> l.contains("Connecting to Takaro")));
    }

    @Test
    void emptyTokenLogsBannerAndDoesNotConnect() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:1/");
        config.setRegistrationToken("");

        new TakaroConnector(adapter, config, Optional.empty()).connect();

        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("registration_token not set")));
        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("no restart needed")));
        assertTrue(adapter.infos.stream().noneMatch(l -> l.contains("Connecting to Takaro")),
                "no token must not open a connection");
    }

    @Test
    void emptyIdentityDoesNotConnect() {
        TestAdapter adapter = new TestAdapter();
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl("ws://127.0.0.1:1/");
        config.setRegistrationToken("reg");
        config.setIdentityToken("");

        new TakaroConnector(adapter, config, Optional.empty()).connect();

        assertTrue(adapter.warnings.stream().anyMatch(l -> l.contains("identity_token not set")));
        assertTrue(adapter.infos.stream().noneMatch(l -> l.contains("Connecting to Takaro")),
                "Takaro rejects an empty identity, so do not try");
    }
}
