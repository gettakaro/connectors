package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into {@code zombie.network.GameServer.disconnectPlayer(IsoPlayer, IConnection)}.
 * Hooked on enter while arg0 (the player) is still populated.
 */
public final class PlayerDisconnectAdvice {

    private PlayerDisconnectAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object player) {
        Bridge.playerDisconnected(player);
    }
}
