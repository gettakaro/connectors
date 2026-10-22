package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Research probe for #201, installed only when {@code TAKARO_COMMAND_PROBE_URL} is set.
 * Inlined into {@code zombie.network.GameServer.receiveReceiveCommand(ByteBufferReader,
 * UdpConnection, short)}; a claimed command skips the method, so the game neither runs it nor
 * sends its own reply.
 */
public final class ReceiveCommandAdvice {

    private ReceiveCommandAdvice() {
    }

    @Advice.OnMethodEnter(skipOn = Advice.OnNonDefaultValue.class, suppress = Throwable.class)
    public static boolean enter(@Advice.Argument(0) Object reader, @Advice.Argument(1) Object connection) {
        return Bridge.receiveCommand(reader, connection);
    }
}
