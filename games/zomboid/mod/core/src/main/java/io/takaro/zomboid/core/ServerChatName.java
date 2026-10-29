package io.takaro.zomboid.core;

/**
 * The sender name on chat the connector sends. Takaro's {@code senderNameOverride} (filled
 * from the server's serverChatName setting by newer Takaro releases) wins over the
 * connector's {@code serverChatName} config; with neither, the message keeps the connector's
 * older rendering: server chat with the game server's name (else "Server") in front of the text.
 */
public final class ServerChatName {
    public static final String DEFAULT_SENDER = "Server";

    private ServerChatName() {}

    /** The name to show as the sender, or {@code null} when none is set. */
    public static String resolve(String senderNameOverride, String configuredName) {
        String name = clean(senderNameOverride);
        return name != null ? name : clean(configuredName);
    }

    /**
     * The text for a message sent without a sender field (game server chat, which shows no
     * author): {@code "<name>: <message>"}, where a {@code null} name falls back to the game
     * server name, then {@code "Server"}.
     */
    public static String render(String senderName, String serverName, String message) {
        String name = senderName;
        if (name == null) {
            name = serverName != null && !serverName.isEmpty() ? serverName : DEFAULT_SENDER;
        }
        return name + ": " + message;
    }

    private static String clean(String name) {
        if (name == null) {
            return null;
        }
        String trimmed = name.trim();
        return trimmed.isEmpty() ? null : trimmed;
    }
}
