package io.takaro.zomboid.agent;

import static net.bytebuddy.matcher.ElementMatchers.isStatic;
import static net.bytebuddy.matcher.ElementMatchers.named;
import static net.bytebuddy.matcher.ElementMatchers.nameStartsWith;
import static net.bytebuddy.matcher.ElementMatchers.takesArguments;

import io.takaro.zomboid.agent.hooks.JoinAdvice;
import io.takaro.zomboid.agent.hooks.TickAdvice;
import java.lang.instrument.Instrumentation;
import java.lang.management.ManagementFactory;
import net.bytebuddy.agent.builder.AgentBuilder;
import net.bytebuddy.asm.Advice;
import net.bytebuddy.description.type.TypeDescription;
import net.bytebuddy.dynamic.DynamicType;
import net.bytebuddy.utility.JavaModule;

/**
 * M0 spike agent: prove that a {@code -javaagent} jar injected through
 * {@code JAVA_TOOL_OPTIONS} loads inside the Project Zomboid B42 dedicated
 * server JVM and that ByteBuddy can instrument the (class-file v69) game
 * classes.
 *
 * <p>Hooks installed here: the main-loop tick
 * ({@code zombie.network.RCONServer.update()}) and the authoritative join
 * ({@code zombie.network.GameServer.receivePlayerConnect}).
 */
public final class TakaroAgent {

    private TakaroAgent() {
    }

    public static void premain(String args, Instrumentation inst) {
        try {
            AgentLog.log("premain: Takaro Project Zomboid connector (M0 spike)");
            AgentLog.log("premain: agent args = " + args);
            AgentLog.log("premain: java.version = " + System.getProperty("java.version")
                    + " vendor = " + System.getProperty("java.vm.vendor")
                    + " vm = " + System.getProperty("java.vm.name"));
            AgentLog.log("premain: jvm input args = " + ManagementFactory.getRuntimeMXBean().getInputArguments());
            AgentLog.log("premain: bytebuddy version = " + byteBuddyVersion()
                    + " from " + net.bytebuddy.ByteBuddy.class.getProtectionDomain().getCodeSource());
            AgentLog.log("premain: instrumentation retransform=" + inst.isRetransformClassesSupported()
                    + " redefine=" + inst.isRedefineClassesSupported());

            install(inst);
            AgentLog.log("premain: hooks installed");
        } catch (Throwable t) {
            AgentLog.error("premain failed", t);
        }
    }

    /** Entry point for dynamic attach; unused in production, handy for debugging. */
    public static void agentmain(String args, Instrumentation inst) {
        premain(args, inst);
    }

    /** Reads the ByteBuddy version stamped into our own jar manifest at build time. */
    private static String byteBuddyVersion() {
        try {
            java.net.URL url = TakaroAgent.class.getProtectionDomain().getCodeSource().getLocation();
            try (java.util.jar.JarFile jar = new java.util.jar.JarFile(new java.io.File(url.toURI()))) {
                return jar.getManifest().getMainAttributes().getValue("ByteBuddy-Version");
            }
        } catch (Throwable ignored) {
            return "unknown";
        }
    }

    private static void install(Instrumentation inst) {
        new AgentBuilder.Default()
                .disableClassFormatChanges()
                .with(AgentBuilder.RedefinitionStrategy.DISABLED)
                .ignore(nameStartsWith("net.bytebuddy.")
                        .or(nameStartsWith("io.takaro."))
                        .or(nameStartsWith("io.takaro.zomboid.libs.")))
                .with(new LoggingListener())
                .type(named("zombie.network.RCONServer"))
                .transform((builder, type, loader, module, pd) -> builder.visit(
                        Advice.to(TickAdvice.class)
                                .on(named("update").and(isStatic()).and(takesArguments(0)))))
                .type(named("zombie.network.GameServer"))
                .transform((builder, type, loader, module, pd) -> builder.visit(
                        Advice.to(JoinAdvice.class)
                                .on(named("receivePlayerConnect").and(isStatic()).and(takesArguments(3)))))
                .installOn(inst);
    }

    /** Logs every transform attempt so a silently-unbound matcher is visible. */
    private static final class LoggingListener implements AgentBuilder.Listener {

        @Override
        public void onDiscovery(String typeName, ClassLoader loader, JavaModule module, boolean loaded) {
            // too noisy to log
        }

        @Override
        public void onTransformation(TypeDescription type, ClassLoader loader, JavaModule module,
                                     boolean loaded, DynamicType dynamicType) {
            AgentLog.log("listener: transformed " + type.getName() + " (loaded=" + loaded
                    + ", loader=" + loader + ")");
        }

        @Override
        public void onIgnored(TypeDescription type, ClassLoader loader, JavaModule module, boolean loaded) {
            // too noisy to log
        }

        @Override
        public void onError(String typeName, ClassLoader loader, JavaModule module, boolean loaded,
                            Throwable throwable) {
            AgentLog.error("listener: transform failed for " + typeName, throwable);
        }

        @Override
        public void onComplete(String typeName, ClassLoader loader, JavaModule module, boolean loaded) {
            // too noisy to log
        }
    }
}
