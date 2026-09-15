package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into {@code zombie.network.chat.ChatServer.sendMessage(ChatMessage)}.
 * Hooked on enter; the argument is passed as {@link Object} so the advice body
 * stays a single decoupled {@link Bridge} call (decoding happens in {@code Pz}).
 */
public final class ChatAdvice {

    private ChatAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object chatMessage) {
        Bridge.chat(chatMessage);
    }
}
