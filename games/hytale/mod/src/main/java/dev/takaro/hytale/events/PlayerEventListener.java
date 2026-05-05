package dev.takaro.hytale.events;

import com.hypixel.hytale.server.core.event.events.player.PlayerConnectEvent;
import com.hypixel.hytale.server.core.event.events.player.PlayerDisconnectEvent;
import com.hypixel.hytale.server.core.io.netty.NettyUtil;
import dev.takaro.hytale.TakaroPlugin;
import io.netty.channel.Channel;

import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.util.HashMap;
import java.util.Map;

/**
 * Listens for player events from Hytale and forwards them to Takaro
 * Uses official Hytale event pattern
 */
public class PlayerEventListener {
    private final TakaroPlugin plugin;
    private final Map<String, Long> lastDisconnectTime = new HashMap<>();
    private static final long DISCONNECT_COOLDOWN_MS = 5000; // 5 seconds

    public PlayerEventListener(TakaroPlugin plugin) {
        this.plugin = plugin;
    }

    /**
     * Handle player connect events
     */
    public void onPlayerConnect(PlayerConnectEvent event) {
        try {
            // Extract player data
            String playerName = event.getPlayerRef().getUsername();
            String uuid = event.getPlayerRef().getUuid().toString();

            // Get player IP address
            String ipAddress = "127.0.0.1"; // Default fallback
            try {
                Channel channel = event.getPlayerRef().getPacketHandler().getChannel();
                SocketAddress remoteAddress = NettyUtil.getRemoteSocketAddress(channel);
                if (remoteAddress instanceof InetSocketAddress) {
                    ipAddress = ((InetSocketAddress) remoteAddress).getAddress().getHostAddress();
                }
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Could not get IP for player " + playerName + ": " + e.getMessage());
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("[EVENT] Player connected: " + playerName + " from " + ipAddress);

            // Build connect event for Takaro
            Map<String, Object> eventData = new HashMap<>();

            Map<String, String> player = new HashMap<>();
            player.put("name", playerName);
            player.put("gameId", uuid);
            player.put("platformId", "hytale:" + uuid);
            player.put("ip", ipAddress); // IP goes inside player object per IGamePlayer schema

            eventData.put("player", player);

            // Send to all Takaro connections (production and dev if enabled)
            plugin.sendGameEventToAll("player-connected", eventData);
            plugin.getLogger().at(java.util.logging.Level.FINE).log("Forwarded player connect to Takaro");

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling player connect: " + e.getMessage());
            e.printStackTrace();
        }
    }

    /**
     * Handle player disconnect events
     */
    public void onPlayerDisconnect(PlayerDisconnectEvent event) {
        try {
            // Extract player data
            String playerName = event.getPlayerRef().getUsername();
            String uuid = event.getPlayerRef().getUuid().toString();

            plugin.getLogger().at(java.util.logging.Level.FINE).log("[EVENT] Player disconnected: " + playerName);

            // Deduplicate - Hytale fires PlayerDisconnectEvent multiple times
            long currentTime = System.currentTimeMillis();
            Long lastTime = lastDisconnectTime.get(uuid);
            if (lastTime != null && (currentTime - lastTime) < DISCONNECT_COOLDOWN_MS) {
                plugin.getLogger().at(java.util.logging.Level.INFO).log("Ignoring duplicate disconnect event for: " + playerName);
                return;
            }
            lastDisconnectTime.put(uuid, currentTime);

            // Build disconnect event for Takaro
            Map<String, Object> eventData = new HashMap<>();

            Map<String, String> player = new HashMap<>();
            player.put("name", playerName);
            player.put("gameId", uuid);
            player.put("platformId", "hytale:" + uuid);

            eventData.put("player", player);

            // Send to all Takaro connections (production and dev if enabled)
            plugin.sendGameEventToAll("player-disconnected", eventData);

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling player disconnect: " + e.getMessage());
            e.printStackTrace();
        }
    }
}
