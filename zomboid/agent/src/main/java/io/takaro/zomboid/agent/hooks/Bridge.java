package io.takaro.zomboid.agent.hooks;

import io.takaro.zomboid.agent.AgentLog;
import io.takaro.zomboid.agent.MainThreadQueue;
import io.takaro.zomboid.agent.PlayerRegistry;
import io.takaro.zomboid.agent.Pz;
import io.takaro.zomboid.agent.Reconciler;
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

    private static volatile MainThreadQueue queue;
    private static volatile Reconciler reconciler;
    private static volatile PlayerRegistry registry;
    private static volatile EventEmitter emitter;
    private static volatile Runnable connectorStarter;
    private static volatile boolean logEvents;

    private static volatile long lastReconcileMs;

    private Bridge() {
    }

    /** Wired up by {@code TakaroAgent.premain} before any hook can fire. */
    public static void configure(MainThreadQueue q, Reconciler r, PlayerRegistry reg,
                                 Runnable starter, boolean logEventsEnabled) {
        queue = q;
        reconciler = r;
        registry = reg;
        connectorStarter = starter;
        logEvents = logEventsEnabled;
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
                + " death=" + FIRED_DEATH.get();
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
            }
        } catch (Throwable t) {
            AgentLog.error("connector start failed", t);
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
}
