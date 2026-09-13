package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into {@code zombie.network.RCONServer.update()V} (public static, called
 * unconditionally once per main-loop iteration on the main thread).
 */
public final class TickAdvice {

    private TickAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter() {
        Bridge.tick();
    }
}
