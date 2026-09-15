package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into {@code zombie.core.logger.ZLogger.write(String logs, String level, boolean append)}
 * — the funnel every public {@code write(..)} overload routes through. Fires for
 * every logger; {@link Bridge#logWrite} filters to the {@code user} logger and is
 * a no-op unless {@code logEvents=true}. {@code this} is the {@code ZLogger}
 * instance, arg0 the line being written.
 */
public final class ZLoggerAdvice {

    private ZLoggerAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.This Object zlogger, @Advice.Argument(0) String line) {
        Bridge.logWrite(zlogger, line);
    }
}
