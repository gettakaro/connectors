package dev.takaro.hytale.events;

import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.event.events.player.PlayerChatEvent;
import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.handlers.ChatFormatter;

import java.awt.Color;
import java.util.HashMap;
import java.util.Map;

/**
 * Listens for chat events from Hytale and forwards them to Takaro
 * Applies player name colors based on Takaro permissions
 * Uses official Hytale event pattern
 */
public class ChatEventListener {
    private final TakaroPlugin plugin;

    public ChatEventListener(TakaroPlugin plugin) {
        this.plugin = plugin;
    }

    /**
     * Handle player chat events
     * Registered via: getEventRegistry().registerGlobal(PlayerChatEvent.class, this::onPlayerChat)
     */
    public void onPlayerChat(PlayerChatEvent event) {
        try {
            // Extract player data
            String playerName = event.getSender().getUsername();
            String uuid = event.getSender().getUuid().toString();
            String message = event.getContent();

            plugin.getLogger().at(java.util.logging.Level.INFO).log("[CHAT] " + playerName + ": " + message);

            // Check if message starts with configured command prefix
            String commandPrefix = plugin.getConfig().getCommandPrefix();
            boolean isCommand = message.startsWith(commandPrefix);

            if (isCommand) {
                // Cancel the event so command doesn't appear in chat (like Hytale's /commands)
                event.setCancelled(true);

                // Send private confirmation message to player
                String commandResponse = plugin.getConfig().getCommandResponse();
                String command = message.substring(commandPrefix.length()); // Extract command without prefix
                String formattedResponse = commandResponse
                    .replace("{prefix}", commandPrefix)
                    .replace("{command}", command);

                Message responseMessage = ChatFormatter.parseColoredMessage(formattedResponse);
                event.getSender().sendMessage(responseMessage);
            } else {
                // Not a command - apply name color formatting for regular chat
                // Get player's name color from cache (set by Takaro via setPlayerNameColor action)
                String nameColorCode = plugin.getPlayerNameColor(uuid);
                Color nameColor = ChatFormatter.parseColor(nameColorCode);

                if (nameColor != null) {
                    // Apply custom name color
                    Message formattedMessage = ChatFormatter.parseColoredMessage(message);
                    event.setFormatter((playerRef, msg) ->
                        Message.join(
                            Message.raw(playerName).color(nameColor),
                            Message.raw(": "),
                            formattedMessage
                        )
                    );
                } else {
                    // No custom color - use default formatting
                    ChatFormatter.onPlayerChat(event);
                }
            }

            // Send ALL messages to Takaro (commands AND regular chat)
            Map<String, Object> chatData = new HashMap<>();
            chatData.put("msg", message);
            chatData.put("channel", "global");

            Map<String, String> player = new HashMap<>();
            player.put("name", playerName);
            player.put("gameId", uuid);
            player.put("platformId", "hytale:" + uuid);

            chatData.put("player", player);

            // Send to all Takaro connections (production and dev if enabled)
            plugin.sendGameEventToAll("chat-message", chatData);
            plugin.getLogger().at(java.util.logging.Level.FINE).log("Forwarded to Takaro: " + message);

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling chat event: " + e.getMessage());
            e.printStackTrace();
        }
    }
}
