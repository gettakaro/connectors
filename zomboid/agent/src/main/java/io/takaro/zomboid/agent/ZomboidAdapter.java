package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.EventEmitter;
import io.takaro.zomboid.core.GameAdapter;
import io.takaro.zomboid.core.model.BanEntry;
import io.takaro.zomboid.core.model.CommandResult;
import io.takaro.zomboid.core.model.GameEntity;
import io.takaro.zomboid.core.model.GameItem;
import io.takaro.zomboid.core.model.GameLocation;
import io.takaro.zomboid.core.model.InventoryItem;
import io.takaro.zomboid.core.model.PlayerInfo;
import io.takaro.zomboid.core.model.PlayerLocation;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The Project Zomboid {@link GameAdapter}. Every query/action runs on the game
 * main thread (the core wraps them through {@link #runOnMainThread}); M1 actions
 * delegate to {@link Pz}. Actions slated for M2 throw
 * {@link UnsupportedOperationException}, which the core turns into a clean
 * protocol error rather than a hang or crash.
 */
public final class ZomboidAdapter implements GameAdapter {

    private final MainThreadQueue queue;
    private final PlayerRegistry registry;
    private final Reconciler reconciler;
    private final BanStore banStore;
    private final boolean debug;

    public ZomboidAdapter(MainThreadQueue queue, PlayerRegistry registry,
                          Reconciler reconciler, BanStore banStore, boolean debug) {
        this.queue = queue;
        this.registry = registry;
        this.reconciler = reconciler;
        this.banStore = banStore;
        this.debug = debug;
    }

    // --- logging / threading ---

    @Override
    public void logInfo(String msg) {
        AgentLog.log(msg);
    }

    @Override
    public void logWarning(String msg) {
        AgentLog.log("WARN " + msg);
    }

    @Override
    public void logDebug(String msg) {
        if (debug) {
            AgentLog.log("DEBUG " + msg);
        }
    }

    @Override
    public void runOnMainThread(Runnable task) {
        queue.runOnMainThread(task);
    }

    @Override
    public void onConnectionEstablished() {
        // Re-seed the registry on the main thread, silently (no event storm).
        queue.runOnMainThread(reconciler::reseed);
    }

    @Override
    public void setEventEmitter(EventEmitter emitter) {
        reconciler.setEmitter(emitter);
        // Chat/death advice emit directly through the Bridge, so it needs the
        // emitter too. The emitter object is stable across reconnects.
        io.takaro.zomboid.agent.hooks.Bridge.setEmitter(emitter);
    }

    // --- player queries (M1) ---

    @Override
    public PlayerInfo getPlayer(String gameId) {
        return Pz.getPlayer(gameId);
    }

    @Override
    public List<PlayerInfo> getPlayers() {
        return Pz.getPlayers();
    }

    @Override
    public PlayerLocation getPlayerLocation(String gameId) {
        return Pz.getPlayerLocation(gameId);
    }

    // --- M2 queries ---

    /** The full item catalogue is large (>1000) and static, so it is cached ~10 min. */
    private static final long ITEM_CACHE_TTL_MS = 10 * 60 * 1000L;
    private volatile List<GameItem> itemCache;
    private volatile long itemCacheAtMs;

    @Override
    public List<InventoryItem> getPlayerInventory(String gameId) {
        List<InventoryItem> items = Pz.getPlayerInventory(gameId);
        if (items == null) {
            throw new IllegalStateException("Player not online: " + gameId);
        }
        return items;
    }

    @Override
    public List<GameItem> listItems() {
        List<GameItem> cached = itemCache;
        if (cached != null && System.currentTimeMillis() - itemCacheAtMs < ITEM_CACHE_TTL_MS) {
            return cached;
        }
        List<GameItem> built = Pz.listItems();
        itemCache = built;
        itemCacheAtMs = System.currentTimeMillis();
        return built;
    }

    @Override
    public List<GameEntity> listEntities() {
        return Pz.listEntities();
    }

    @Override
    public List<GameLocation> listLocations() {
        return Pz.listLocations();
    }

    @Override
    public void shutdownServer() {
        Pz.shutdown();
    }

    // --- player actions (M1) ---

    @Override
    public void giveItem(String gameId, String itemCode, int amount, String quality) {
        String error = Pz.giveItem(gameId, itemCode, amount);
        if (error != null) {
            throw new IllegalStateException(error);
        }
    }

    @Override
    public String getServerName() {
        return Pz.serverName();
    }

    @Override
    public void sendMessage(String message, String recipientGameId) {
        if (recipientGameId == null || recipientGameId.isEmpty()) {
            Pz.sendMessageGlobal(message);
        } else if (!Pz.sendMessageTo(recipientGameId, message)) {
            throw new IllegalStateException("Recipient not online: " + recipientGameId);
        }
    }

    @Override
    public CommandResult executeConsoleCommand(String command) {
        return Pz.rcon(command);
    }

    @Override
    public void teleportPlayer(String gameId, double x, double y, double z, String dimension) {
        if (!Pz.teleport(gameId, x, y, z)) {
            throw new IllegalStateException("No such user: " + gameId);
        }
    }

    @Override
    public void kickPlayer(String gameId, String reason) {
        if (!Pz.kick(gameId, reason)) {
            throw new IllegalStateException("Player not online: " + gameId);
        }
    }

    @Override
    public void banPlayer(String gameId, String reason, String expiresAt) {
        long steamId = Pz.banUser(gameId, reason);
        String name = gameId;
        PlayerInfo known = registry.get(gameId);
        if (known != null) {
            name = known.name();
        }
        banStore.put(gameId, steamId != 0L ? Long.toString(steamId) : null, name, reason, expiresAt);
        // kick if currently online; ignore failure (offline ban is fine)
        Pz.kick(gameId, reason == null || reason.isEmpty() ? "Banned by Takaro" : reason);
    }

    @Override
    public void unbanPlayer(String gameId) {
        BanStore.Ban existing = banStore.get(gameId);
        String steamId = existing != null ? existing.steamId : null;
        Pz.unbanUser(gameId, steamId);
        banStore.remove(gameId);
    }

    @Override
    public List<BanEntry> listBans() {
        List<BanEntry> out = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (BanStore.Ban b : banStore.list()) {
            out.add(new BanEntry(b.gameId, b.name, b.reason, b.expiresAt));
            seen.add(b.gameId);
            if (b.steamId != null) {
                seen.add(b.steamId);
            }
        }
        for (BanEntry row : Pz.bannedSteamIdRows()) {
            if (seen.add(row.gameId())) {
                out.add(row);
            }
        }
        for (String username : Pz.bannedUsernamesViaConn()) {
            if (seen.add(username)) {
                out.add(new BanEntry(username, username, null, null));
            }
        }
        return out;
    }
}
