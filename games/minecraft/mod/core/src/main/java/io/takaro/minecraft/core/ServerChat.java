package io.takaro.minecraft.core;

import io.takaro.minecraft.core.model.ChatLine;

/**
 * Who Takaro's messages come from. The order is: {@code opts.senderNameOverride} (Takaro fills it
 * from the server's chat name setting), then the connector's {@code server_chat_name} setting, then
 * no sender at all, which keeps the bare text.
 */
public final class ServerChat {

    private ServerChat() {}

    /** The sender name, or null when neither value is set. Blank counts as not set. */
    public static String resolveSender(String override, String configured) {
        String name = trimmed(override);
        return name != null ? name : trimmed(configured);
    }

    public static ChatLine line(String message, String recipientGameId, String override, String configured) {
        return new ChatLine(message, resolveSender(override, configured), recipientGameId != null);
    }

    private static String trimmed(String value) {
        if (value == null) return null;
        String t = value.strip();
        return t.isEmpty() ? null : t;
    }
}
