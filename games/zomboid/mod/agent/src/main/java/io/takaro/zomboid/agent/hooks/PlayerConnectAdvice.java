package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into
 * {@code zombie.network.GameServer.receivePlayerConnect(ByteBufferReader, IConnection, String)}.
 * Hooked on exit so the player is actually accepted before we flag the join.
 */
public final class PlayerConnectAdvice {

    private PlayerConnectAdvice() {
    }

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit(@Advice.Argument(2) String username) {
        Bridge.playerConnected(username);
    }
}
