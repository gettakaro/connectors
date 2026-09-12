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
import io.takaro.zomboid.agent.hooks.TickAdvice;

import java.lang.instrument.Instrumentation;

import net.bytebuddy.agent.builder.AgentBuilder;
import net.bytebuddy.asm.Advice;
import net.bytebuddy.description.type.TypeDescription;
import net.bytebuddy.dynamic.DynamicType;
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

    private HookInstaller() {
    }

    public static boolean sawTickTarget() {
        return sawTickTarget;
    }

    public static void install(Instrumentation inst) {
        new AgentBuilder.Default()
                .disableClassFormatChanges()
                .with(AgentBuilder.RedefinitionStrategy.DISABLED)
                .ignore(nameStartsWith("net.bytebuddy.")
                        .or(nameStartsWith("io.takaro."))
                        .or(nameStartsWith("io.takaro.zomboid.libs.")))
                .with(new LoggingListener())
                // tick — main-loop drain + reconcile + connector start
                .type(named("zombie.network.RCONServer"))
                .transform((builder, type, loader, module, pd) -> builder.visit(
                        Advice.to(TickAdvice.class)
                                .on(named("update").and(isStatic()).and(takesArguments(0)))))
                // join + leave
                .type(named("zombie.network.GameServer"))
                .transform((builder, type, loader, module, pd) -> builder
                        .visit(Advice.to(PlayerConnectAdvice.class)
                                .on(named("receivePlayerConnect").and(isStatic()).and(takesArguments(3))))
                        .visit(Advice.to(PlayerDisconnectAdvice.class)
                                .on(named("disconnectPlayer").and(isStatic()).and(takesArguments(2)))))
                // chat
                .type(named("zombie.network.chat.ChatServer"))
                .transform((builder, type, loader, module, pd) -> builder.visit(
                        Advice.to(ChatAdvice.class)
                                .on(named("sendMessage")
                                        .and(takesArgument(0, named("zombie.chat.ChatMessage"))))))
                // death
                .type(named("zombie.characters.IsoPlayer"))
                .transform((builder, type, loader, module, pd) -> builder.visit(
                        Advice.to(PlayerDeathAdvice.class)
                                .on(named("onKilled").and(takesArguments(3)))))
                .installOn(inst);
    }

    /** Logs every transform attempt so a silently-unbound matcher is visible. */
    private static final class LoggingListener implements AgentBuilder.Listener {

        @Override
        public void onDiscovery(String typeName, ClassLoader loader, JavaModule module, boolean loaded) {
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
