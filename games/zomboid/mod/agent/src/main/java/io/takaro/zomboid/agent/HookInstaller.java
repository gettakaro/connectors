package io.takaro.zomboid.agent;

import static net.bytebuddy.matcher.ElementMatchers.isStatic;
import static net.bytebuddy.matcher.ElementMatchers.named;
import static net.bytebuddy.matcher.ElementMatchers.nameStartsWith;
import static net.bytebuddy.matcher.ElementMatchers.takesArgument;
import static net.bytebuddy.matcher.ElementMatchers.takesArguments;

import io.takaro.zomboid.agent.hooks.ChatAdvice;
import io.takaro.zomboid.agent.hooks.PlayerConnectAdvice;
import io.takaro.zomboid.agent.hooks.PlayerDeathAdvice;
import io.takaro.zomboid.agent.hooks.PlayerDisconnectAdvice;
import io.takaro.zomboid.agent.hooks.ReceiveCommandAdvice;
import io.takaro.zomboid.agent.hooks.TickAdvice;
import io.takaro.zomboid.agent.hooks.ZLoggerAdvice;
import io.takaro.zomboid.agent.hooks.ZombieKilledAdvice;
import io.takaro.zomboid.agent.research.CommandProbeRuntime;

import java.lang.instrument.Instrumentation;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

import net.bytebuddy.agent.builder.AgentBuilder;
import net.bytebuddy.asm.Advice;
import net.bytebuddy.description.method.MethodDescription;
import net.bytebuddy.description.type.TypeDescription;
import net.bytebuddy.dynamic.DynamicType;
import net.bytebuddy.matcher.ElementMatcher;
import net.bytebuddy.utility.JavaModule;

/**
 * Installs the five ByteBuddy hooks and tracks which game classes were actually
 * transformed, so a watchdog can fail loud if the tick hook never binds.
 *
 * <p>Per the M0 findings, {@link AgentBuilder.Listener#onTransformation} is NOT
 * proof a method matcher bound (it fires even for zero matches). Real binding is
 * proven by the first-invocation sentinels in {@code Bridge}; this installer
 * only records that the carrier <em>type</em> was seen, which the watchdog
 * combines with the tick sentinel.
 */
public final class HookInstaller {

    private static volatile boolean sawTickTarget;   // RCONServer transformed
    private static volatile boolean sawGameServer;   // GameServer transformed
    private static final Set<String> UNBOUND = ConcurrentHashMap.newKeySet();

    private HookInstaller() {
    }

    public static boolean sawTickTarget() {
        return sawTickTarget;
    }

    public static List<String> unboundHooks() {
        return UNBOUND.stream().sorted().toList();
    }

    static List<String> unbound(
            TypeDescription type,
            Map<String, ElementMatcher<? super MethodDescription>> hooks) {
        return hooks.entrySet().stream()
                .filter(hook -> type.getDeclaredMethods().stream().noneMatch(hook.getValue()::matches))
                .map(Map.Entry::getKey)
                .sorted()
                .toList();
    }

    private static void recordUnbound(
            TypeDescription type,
            Map<String, ElementMatcher<? super MethodDescription>> hooks) {
        for (String hook : unbound(type, hooks)) {
            UNBOUND.add(hook);
            AgentLog.error("hook " + hook + ": matcher bound nothing on " + type.getName());
        }
    }

    public static void install(Instrumentation inst) {
        new AgentBuilder.Default()
                .disableClassFormatChanges()
                .with(AgentBuilder.RedefinitionStrategy.RETRANSFORMATION)
                .with(AgentBuilder.RedefinitionStrategy.DiscoveryStrategy.Reiterating.INSTANCE)
                .ignore(nameStartsWith("net.bytebuddy.")
                        .or(nameStartsWith("io.takaro."))
                        .or(nameStartsWith("io.takaro.zomboid.libs.")))
                .with(new LoggingListener())
                // tick — main-loop drain + reconcile + connector start
                .type(named("zombie.network.RCONServer"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> tick =
                            named("update").and(isStatic()).and(takesArguments(0));
                    recordUnbound(type, Map.of("tick", tick));
                    return builder.visit(Advice.to(TickAdvice.class).on(tick));
                })
                // join + leave
                .type(named("zombie.network.GameServer"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> connect =
                            named("receivePlayerConnect").and(isStatic()).and(takesArguments(3));
                    ElementMatcher.Junction<MethodDescription> disconnect =
                            named("disconnectPlayer").and(isStatic()).and(takesArguments(2));
                    recordUnbound(type, Map.of("connect", connect, "disconnect", disconnect));
                    DynamicType.Builder<?> b = builder
                            .visit(Advice.to(PlayerConnectAdvice.class).on(connect))
                            .visit(Advice.to(PlayerDisconnectAdvice.class).on(disconnect));
                    if (!CommandProbeRuntime.enabled()) {
                        return b;
                    }
                    // #201 research probe only; off unless TAKARO_COMMAND_PROBE_URL is set.
                    ElementMatcher.Junction<MethodDescription> command = named("receiveReceiveCommand")
                            .and(isStatic()).and(takesArguments(3));
                    recordUnbound(type, Map.of("command-probe", command));
                    return b.visit(Advice.to(ReceiveCommandAdvice.class).on(command));
                })
                // chat
                .type(named("zombie.network.chat.ChatServer"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> chat = named("sendMessage")
                            .and(takesArgument(0, named("zombie.chat.ChatMessage")));
                    recordUnbound(type, Map.of("chat", chat));
                    return builder.visit(Advice.to(ChatAdvice.class).on(chat));
                })
                // death
                .type(named("zombie.characters.IsoPlayer"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> death =
                            named("onKilled").and(takesArguments(3));
                    recordUnbound(type, Map.of("death", death));
                    return builder.visit(Advice.to(PlayerDeathAdvice.class).on(death));
                })
                // entity-killed (zombie killed by a player)
                .type(named("zombie.characters.IsoZombie"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> zombieKilled =
                            named("onKilled").and(takesArguments(3));
                    recordUnbound(type, Map.of("zombie-killed", zombieKilled));
                    return builder.visit(Advice.to(ZombieKilledAdvice.class).on(zombieKilled));
                })
                // log (user logger writes) — emission gated on logEvents in the Bridge.
                // Hook the write(String,String,boolean) funnel every public
                // write(..) overload routes through (the 1-arg form is rarely called).
                .type(named("zombie.core.logger.ZLogger"))
                .transform((builder, type, loader, module, pd) -> {
                    ElementMatcher.Junction<MethodDescription> log = named("write").and(takesArguments(3))
                            .and(takesArgument(0, named("java.lang.String")))
                            .and(takesArgument(2, boolean.class));
                    recordUnbound(type, Map.of("log", log));
                    return builder.visit(Advice.to(ZLoggerAdvice.class).on(log));
                })
                .installOn(inst);
        retransformLateTargets(inst);
    }

    /**
     * RETRANSFORMATION only retransforms classes already loaded when the agent
     * installs. Some carrier types (notably {@code zombie.characters.IsoZombie},
     * which loads when the first zombie spawns — after a player joins) load later
     * and can slip past the on-load path. This watchdog waits for each late type
     * to appear in the loaded set and explicitly retransforms it, which re-runs
     * the (retransform-capable) transformer and binds the advice. Idempotent:
     * retransforming an already-instrumented class is harmless.
     */
    private static void retransformLateTargets(Instrumentation inst) {
        final String[] wanted = {
                "zombie.characters.IsoZombie",
                "zombie.characters.IsoPlayer",
                "zombie.network.chat.ChatServer",
        };
        Thread t = new Thread(() -> {
            java.util.Set<String> done = new java.util.HashSet<>();
            long end = System.currentTimeMillis() + 15 * 60 * 1000L;
            while (System.currentTimeMillis() < end && done.size() < wanted.length) {
                for (Class<?> c : inst.getAllLoadedClasses()) {
                    String n = c.getName();
                    for (String w : wanted) {
                        if (!done.contains(w) && w.equals(n)) {
                            done.add(w);
                            try {
                                if (inst.isModifiableClass(c)) {
                                    inst.retransformClasses(c);
                                    AgentLog.log("watchdog: retransformed " + w);
                                } else {
                                    AgentLog.log("watchdog: " + w + " not modifiable");
                                }
                            } catch (Throwable ex) {
                                AgentLog.error("watchdog retransform " + w, ex);
                            }
                        }
                    }
                }
                try {
                    Thread.sleep(2000L);
                } catch (InterruptedException ie) {
                    return;
                }
            }
        }, "takaro-retransform-watchdog");
        t.setDaemon(true);
        t.start();
    }

    /** Logs every transform attempt so a silently-unbound matcher is visible. */
    private static final class LoggingListener implements AgentBuilder.Listener {

        @Override
        public void onDiscovery(String typeName, ClassLoader loader, JavaModule module, boolean loaded) {
            if (typeName.startsWith("zombie.characters.Iso") || typeName.startsWith("zombie.network.")) {
                AgentLog.log("listener: discovery " + typeName + " (loaded=" + loaded + ")");
            }
        }

        @Override
        public void onTransformation(TypeDescription type, ClassLoader loader, JavaModule module,
                                     boolean loaded, DynamicType dynamicType) {
            String name = type.getName();
            if ("zombie.network.RCONServer".equals(name)) {
                sawTickTarget = true;
            } else if ("zombie.network.GameServer".equals(name)) {
                sawGameServer = true;
            }
            AgentLog.log("listener: transformed " + name + " (loaded=" + loaded + ")");
        }

        @Override
        public void onIgnored(TypeDescription type, ClassLoader loader, JavaModule module, boolean loaded) {
            String name = type.getName();
            if (name.equals("zombie.characters.IsoZombie")) {
                AgentLog.log("listener: IGNORED " + name + " (loaded=" + loaded + ")");
            }
        }

        @Override
        public void onError(String typeName, ClassLoader loader, JavaModule module, boolean loaded,
                            Throwable throwable) {
            AgentLog.error("listener: transform failed for " + typeName, throwable);
        }

        @Override
        public void onComplete(String typeName, ClassLoader loader, JavaModule module, boolean loaded) {
        }
    }
}
