package io.takaro.zomboid.agent.hooks;

import io.takaro.zomboid.agent.AgentLog;
import io.takaro.zomboid.agent.EventManagerCallback;
import io.takaro.zomboid.agent.MainThreadQueue;
import io.takaro.zomboid.agent.PlayerRegistry;
import io.takaro.zomboid.agent.Pz;
import io.takaro.zomboid.agent.Reconciler;
import io.takaro.zomboid.agent.TokenBucket;
import io.takaro.zomboid.core.EventEmitter;
import io.takaro.zomboid.core.model.PlayerInfo;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The single static entry point that instrumented game code calls into.
 *
 * <p>Every advice body is exactly one call to a method here, and every method
 * here wraps its whole body in {@code try/catch(Throwable)}: an exception must
 * never escape into a game code path. This class has no static initialiser that
 * touches game classes, so loading it can never fail mid-tick.
 *
 * <p>The connector is started from the FIRST tick (not from premain), because
 * the image launches three JVMs and only the one that actually ticks is the
 * real server on its main thread with the game classes loaded.
 */
public final class Bridge {

    private static final long RECONCILE_INTERVAL_MS = 2000L;

    private static final AtomicLong TICKS = new AtomicLong();
    private static final AtomicBoolean CONNECTOR_STARTED = new AtomicBoolean();

    // First-invocation sentinels per hook — real proof a matcher bound
    // (Listener.onTransformation is not, it fires even for zero matches).
    private static final AtomicBoolean FIRED_TICK = new AtomicBoolean();
    private static final AtomicBoolean FIRED_CHAT = new AtomicBoolean();
    private static final AtomicBoolean FIRED_CONNECT = new AtomicBoolean();
    private static final AtomicBoolean FIRED_DISCONNECT = new AtomicBoolean();
    private static final AtomicBoolean FIRED_DEATH = new AtomicBoolean();
    private static final AtomicBoolean FIRED_ZOMBIE_KILLED = new AtomicBoolean();
    private static final AtomicBoolean FIRED_LOG = new AtomicBoolean();

    // Rate limiters for the firehose events (a horde or a chatty log cannot flood
    // the WebSocket). entity-killed: 20/s, burst 50 (per plan). log: 30/s, burst 60.
    private static final TokenBucket ZOMBIE_KILL_LIMITER = new TokenBucket(50, 20.0);
    private static final TokenBucket LOG_LIMITER = new TokenBucket(60, 30.0);
    private static final AtomicBoolean ZOMBIE_KILL_DROP_WARNED = new AtomicBoolean();
    private static final AtomicBoolean LOG_DROP_WARNED = new AtomicBoolean();
    private static final AtomicLong ZOMBIE_KILL_DROPPED = new AtomicLong();
    private static final AtomicLong LOG_DROPPED = new AtomicLong();

    // EventManager log callback registration (attempted from the tick once the
    // game is up; server-side observable, independent of logEvents).
    private static final AtomicBoolean LOG_CALLBACK_REGISTERED = new AtomicBoolean();
    private static final AtomicBoolean LOG_CALLBACK_GAVE_UP = new AtomicBoolean();

    private static volatile MainThreadQueue queue;
    private static volatile Reconciler reconciler;
    private static volatile PlayerRegistry registry;
    private static volatile EventEmitter emitter;
    private static volatile Runnable connectorStarter;
    private static volatile boolean logEvents;
    private static volatile boolean debugCatalog;

    private static volatile long lastReconcileMs;

    private Bridge() {
    }

    /** Wired up by {@code TakaroAgent.premain} before any hook can fire. */
    public static void configure(MainThreadQueue q, Reconciler r, PlayerRegistry reg,
                                 Runnable starter, boolean logEventsEnabled, boolean debugCatalogEnabled) {
        queue = q;
        reconciler = r;
        registry = reg;
        connectorStarter = starter;
        logEvents = logEventsEnabled;
        debugCatalog = debugCatalogEnabled;
    }

    /** Set once the WebSocket client exists (initial connect); also survives reconnects. */
    public static void setEmitter(EventEmitter e) {
        emitter = e;
    }

    public static long tickCount() {
        return TICKS.get();
    }

    public static String hooksFiredSummary() {
        return "tick=" + FIRED_TICK.get()
                + " connect=" + FIRED_CONNECT.get()
                + " disconnect=" + FIRED_DISCONNECT.get()
                + " chat=" + FIRED_CHAT.get()
                + " death=" + FIRED_DEATH.get()
                + " zombieKilled=" + FIRED_ZOMBIE_KILLED.get()
                + " log=" + FIRED_LOG.get();
    }

    // --- hook entry points ---

    /** {@code zombie.network.RCONServer.update()} — main thread, once per tick. */
    public static void tick() {
        try {
            long n = TICKS.incrementAndGet();
            if (FIRED_TICK.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: tick (RCONServer.update) firing on thread="
                        + Thread.currentThread().getName());
                MainThreadQueue q = queue;
                if (q != null) {
                    q.setMainThread(Thread.currentThread());
                }
                startConnectorOnce();
            }

            MainThreadQueue q = queue;
            if (q != null) {
                q.drain();
            }

            registerLogCallbackOnce();

            Reconciler r = reconciler;
            if (r != null) {
                long now = System.currentTimeMillis();
                if (r.isDirty() || now - lastReconcileMs >= RECONCILE_INTERVAL_MS) {
                    lastReconcileMs = now;
                    r.tick();
                }
            }

            if (n % 6000L == 0L) {
                AgentLog.log("tick count=" + n + " hooks[" + hooksFiredSummary() + "]");
            }
        } catch (Throwable t) {
            // never propagate into the game main loop
        }
    }

    private static void startConnectorOnce() {
        try {
            if (CONNECTOR_STARTED.compareAndSet(false, true)) {
                Runnable starter = connectorStarter;
                if (starter != null) {
                    AgentLog.log("first tick reached — starting Takaro connector");
                    starter.run();
                }
                if (debugCatalog) {
                    dumpCatalogSizes();
                }
            }
        } catch (Throwable t) {
            AgentLog.error("connector start failed", t);
        }
    }

    /** One-shot diagnostic (behind {@code debugCatalog}) run on the first tick / main thread. */
    private static void dumpCatalogSizes() {
        try {
            java.util.List<io.takaro.zomboid.core.model.GameItem> items = Pz.listItems();
            int rawRows = Pz.itemRows().size();
            long baseCount = items.stream().filter(i -> i.code() != null && i.code().startsWith("Base.")).count();
            long distinct = items.stream().map(io.takaro.zomboid.core.model.GameItem::code).distinct().count();
            StringBuilder sample = new StringBuilder();
            for (int i = 0; i < Math.min(3, items.size()); i++) {
                if (i > 0) {
                    sample.append(", ");
                }
                sample.append(items.get(i).code());
            }
            AgentLog.log("debugCatalog: listItems=" + items.size()
                    + " (rawRows=" + rawRows + " distinctCodes=" + distinct
                    + " baseCodes=" + baseCount + " sample=[" + sample + "])"
                    + " listEntities=" + Pz.listEntities().size()
                    + " listLocations=" + Pz.listLocations().size());
        } catch (Throwable t) {
            AgentLog.error("debugCatalog dump failed", t);
        }
    }

    /** Register the EventManager log callback once the game instance exists. */
    private static void registerLogCallbackOnce() {
        if (LOG_CALLBACK_REGISTERED.get() || LOG_CALLBACK_GAVE_UP.get()) {
            return;
        }
        try {
            if (Pz.registerLogCallback(new EventManagerCallback())) {
                if (LOG_CALLBACK_REGISTERED.compareAndSet(false, true)) {
                    AgentLog.log("EventManager callback registered (log event source)");
                }
            } else if (TICKS.get() > 6000L) {
                // EventManager never appeared after ~minutes of ticks — stop trying.
                if (LOG_CALLBACK_GAVE_UP.compareAndSet(false, true)) {
                    AgentLog.log("WARN EventManager.instance() still null after 6000 ticks; "
                            + "log event falls back to the ZLogger.write hook only");
                }
            }
        } catch (Throwable t) {
            if (LOG_CALLBACK_GAVE_UP.compareAndSet(false, true)) {
                AgentLog.error("EventManager callback registration failed", t);
            }
        }
    }

    /** {@code zombie.network.chat.ChatServer.sendMessage(ChatMessage)} — chat-message event. */
    public static void chat(Object chatMessage) {
        try {
            if (FIRED_CHAT.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: chat (ChatServer.sendMessage)");
            }
            EventEmitter e = emitter;
            if (e == null || chatMessage == null) {
                return;
            }
            if (Pz.chatIsServerAuthor(chatMessage)) {
                return; // our own outbound / server announcements
            }
            String author = Pz.chatAuthor(chatMessage);
            if (author == null) {
                return;
            }
            String text = Pz.chatText(chatMessage);
            String channel = Pz.chatChannel(chatMessage);
            String name = author;
            PlayerRegistry reg = registry;
            if (reg != null) {
                PlayerInfo known = reg.get(author);
                if (known != null) {
                    name = known.name();
                }
            }
            e.emitChatMessage(author, name, channel, text);
        } catch (Throwable t) {
            // never propagate into the chat path
        }
    }

    /** {@code GameServer.receivePlayerConnect(..)} on exit — flag an immediate reconcile. */
    public static void playerConnected(String username) {
        try {
            if (FIRED_CONNECT.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: connect (GameServer.receivePlayerConnect)");
            }
            AgentLog.log("join detected username=" + username);
            Reconciler r = reconciler;
            if (r != null) {
                r.markDirty();
            }
        } catch (Throwable t) {
            // never propagate into the networking path
        }
    }

    /** {@code GameServer.disconnectPlayer(IsoPlayer, IConnection)} on enter. */
    public static void playerDisconnected(Object player) {
        try {
            if (FIRED_DISCONNECT.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: disconnect (GameServer.disconnectPlayer)");
            }
            Reconciler r = reconciler;
            if (r != null) {
                r.markDirty();
            }
        } catch (Throwable t) {
            // never propagate
        }
    }

    /** {@code IsoPlayer.onKilled(killer, weapon, gory)} on enter — player-death event. */
    public static void playerDeath(Object victim, Object killer) {
        try {
            if (FIRED_DEATH.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: death (IsoPlayer.onKilled)");
            }
            EventEmitter e = emitter;
            if (e == null || victim == null || !Pz.isIsoPlayer(victim)) {
                return;
            }
            String gameId = Pz.playerUsername(victim);
            String name = Pz.playerDisplayName(victim);
            double[] pos = Pz.playerPos(victim);

            String attackerId = null;
            String attackerName = null;
            if (killer != null && Pz.isIsoPlayer(killer)) {
                attackerId = Pz.playerUsername(killer);
                attackerName = Pz.playerDisplayName(killer);
            }
            AgentLog.log("event: player-death " + gameId
                    + (attackerId != null ? " (killed by " + attackerId + ")" : ""));
            e.emitPlayerDeath(gameId, name, attackerId, attackerName, pos[0], pos[1], pos[2], null);
        } catch (Throwable t) {
            // never propagate into the death path
        }
    }

    /**
     * {@code IsoZombie.onKilled(killer, weapon, gory)} on enter — entity-killed
     * event, but only when the killer is a player. Rate-limited (token bucket
     * 20/s, burst 50) so a horde kill does not flood Takaro.
     */
    public static void zombieKilled(Object killer, Object weapon) {
        try {
            if (FIRED_ZOMBIE_KILLED.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: zombie killed (IsoZombie.onKilled)");
            }
            EventEmitter e = emitter;
            if (e == null || killer == null || !Pz.isIsoPlayer(killer)) {
                return; // only player kills are reported
            }
            if (!ZOMBIE_KILL_LIMITER.tryAcquire()) {
                long dropped = ZOMBIE_KILL_DROPPED.incrementAndGet();
                if (ZOMBIE_KILL_DROP_WARNED.compareAndSet(false, true)) {
                    AgentLog.log("WARN entity-killed events are being rate-limited (20/s, burst 50); "
                            + "further drops are counted silently (first drop at total=" + dropped + ")");
                }
                return;
            }
            String gameId = Pz.playerUsername(killer);
            String name = Pz.playerDisplayName(killer);
            String weaponCode = Pz.weaponFromKill(weapon, killer);
            e.emitEntityKilled(gameId, name, "Zombie", weaponCode);
        } catch (Throwable t) {
            // never propagate into the death path
        }
    }

    /** Text-only server report line from the EventManager callback — {@code log} event. */
    public static void logLine(String message) {
        try {
            if (FIRED_LOG.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: log (EventManager callback)");
            }
            emitLogGated(message);
        } catch (Throwable t) {
            // never propagate
        }
    }

    /**
     * {@code ZLogger.write(String)} on enter — secondary {@code log} source,
     * filtered to the {@code user} logger. Fires for every logger write, so the
     * cheap {@code logEvents} gate comes first.
     */
    public static void logWrite(Object zlogger, String line) {
        try {
            if (FIRED_LOG.compareAndSet(false, true)) {
                AgentLog.log("HOOK CONFIRMED: log (ZLogger.write)");
            }
            if (!logEvents) {
                return;
            }
            if (!Pz.isUserLogger(zlogger)) {
                return;
            }
            emitLogGated(line);
        } catch (Throwable t) {
            // never propagate into the logging path
        }
    }

    private static void emitLogGated(String message) {
        if (!logEvents) {
            return;
        }
        EventEmitter e = emitter;
        if (e == null || message == null || message.isEmpty()) {
            return;
        }
        if (!LOG_LIMITER.tryAcquire()) {
            long dropped = LOG_DROPPED.incrementAndGet();
            if (LOG_DROP_WARNED.compareAndSet(false, true)) {
                AgentLog.log("WARN log events are being rate-limited (30/s, burst 60); "
                        + "further drops are counted silently (first drop at total=" + dropped + ")");
            }
            return;
        }
        e.emitLog(message);
    }
}
