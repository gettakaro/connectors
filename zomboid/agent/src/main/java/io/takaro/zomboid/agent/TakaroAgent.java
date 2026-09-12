package io.takaro.zomboid.agent;

import io.takaro.zomboid.agent.hooks.Bridge;
import io.takaro.zomboid.core.TakaroConfig;
import io.takaro.zomboid.core.TakaroConnector;

import java.lang.instrument.Instrumentation;
import java.lang.management.ManagementFactory;

/**
 * Project Zomboid Takaro connector agent ({@code Premain-Class}).
 *
 * <p>The connector is injected through {@code JAVA_TOOL_OPTIONS}, so
 * {@code premain} runs in every JVM the image's launch chain spawns (three of
 * them). It therefore does NOT open the Takaro WebSocket here — it only wires up
 * the runtime and installs the hooks. The connector is started from the FIRST
 * {@code RCONServer.update()} tick, which is guaranteed to be the real server
 * JVM, on its main thread, with the game classes loaded (see {@link Bridge}).
 */
public final class TakaroAgent {

    /** Seconds after the tick target is transformed before we WARN about a dead tick hook. */
    private static final long TICK_WATCHDOG_SECONDS = 180L;

    private TakaroAgent() {
    }

    public static void premain(String args, Instrumentation inst) {
        try {
            AgentLog.log("premain: Takaro Project Zomboid connector (M1)");
            AgentLog.log("premain: java.version=" + System.getProperty("java.version")
                    + " vendor=" + System.getProperty("java.vm.vendor"));
            AgentLog.log("premain: jvm input args = "
                    + ManagementFactory.getRuntimeMXBean().getInputArguments());
            AgentLog.log("premain: instrumentation retransform=" + inst.isRetransformClassesSupported()
                    + " redefine=" + inst.isRedefineClassesSupported());

            // --- load config (file + env overrides; env wins) ---
            ConfigLoader loader = new ConfigLoader();
            TakaroConfig config = loader.load();
            AgentLog.log("premain: wsUrl=" + config.getWsUrl()
                    + " identity=" + (config.getIdentityToken() != null ? "set" : "MISSING")
                    + " registration=" + (config.getRegistrationToken() != null ? "set" : "MISSING")
                    + " debug=" + config.isDebugEnabled()
                    + " logEvents=" + loader.isLogEvents());

            // --- build the runtime graph ---
            MainThreadQueue queue = new MainThreadQueue();
            PlayerRegistry registry = new PlayerRegistry();
            BanStore banStore = new BanStore();
            Reconciler reconciler = new Reconciler(registry, banStore);
            ZomboidAdapter adapter = new ZomboidAdapter(queue, registry, reconciler, banStore,
                    config.isDebugEnabled());
            TakaroConnector connector = new TakaroConnector(adapter, config);

            // Connector is started from the first tick (right JVM + main thread).
            Runnable starter = connector::connect;
            Bridge.configure(queue, reconciler, registry, starter, loader.isLogEvents());

            // --- install hooks (binds only in the JVM that loads the game classes) ---
            HookInstaller.install(inst);
            AgentLog.log("premain: hooks installed");

            startTickWatchdog();

            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                try {
                    connector.shutdown();
                } catch (Throwable ignored) {
                    // best effort on JVM shutdown
                }
            }, "takaro-shutdown"));
        } catch (Throwable t) {
            AgentLog.error("premain failed", t);
        }
    }

    /** Entry point for dynamic attach; handy for debugging. */
    public static void agentmain(String args, Instrumentation inst) {
        premain(args, inst);
    }

    /**
     * Loud failure if the tick hook never fires even though its carrier class
     * ({@code RCONServer}) was transformed — i.e. the matcher bound nothing. In
     * the probe JVMs the game classes are never loaded, so the watchdog stays
     * silent there and only speaks in the real server JVM.
     */
    private static void startTickWatchdog() {
        Thread t = new Thread(() -> {
            long deadlineHit = 0L;
            try {
                while (true) {
                    Thread.sleep(5000L);
                    if (Bridge.tickCount() > 0L) {
                        return; // tick hook confirmed — nothing to warn about
                    }
                    if (HookInstaller.sawTickTarget()) {
                        if (deadlineHit == 0L) {
                            deadlineHit = System.currentTimeMillis() + TICK_WATCHDOG_SECONDS * 1000L;
                        } else if (System.currentTimeMillis() >= deadlineHit) {
                            AgentLog.log("WARN ***** tick hook on RCONServer.update() has NOT fired "
                                    + TICK_WATCHDOG_SECONDS + "s after the class was transformed. "
                                    + "The matcher likely bound nothing — the connector will NOT start. "
                                    + "Re-pin the hook with dump-signatures.py. *****");
                            return;
                        }
                    }
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }, "takaro-tick-watchdog");
        t.setDaemon(true);
        t.start();
    }
}
