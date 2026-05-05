package dev.takaro.hytale.handlers;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.logger.backend.HytaleLoggerBackend;
import com.hypixel.hytale.math.vector.Vector3d;
import com.hypixel.hytale.math.vector.Vector3f;
import com.hypixel.hytale.server.core.HytaleServer;
import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.asset.type.item.config.Item;
import com.hypixel.hytale.server.core.console.ConsoleSender;
import com.hypixel.hytale.server.core.entity.entities.Player;
import com.hypixel.hytale.server.core.inventory.ItemStack;
import com.hypixel.hytale.server.core.inventory.transaction.ItemStackTransaction;
import com.hypixel.hytale.server.core.modules.entity.component.TransformComponent;
import com.hypixel.hytale.server.core.modules.entity.teleport.Teleport;
import com.hypixel.hytale.server.core.universe.PlayerRef;
import com.hypixel.hytale.server.core.universe.world.World;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;
import dev.takaro.hytale.TakaroPlugin;
import dev.takaro.hytale.api.HytaleApiClient;

import java.io.IOException;
import java.util.*;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.logging.LogRecord;
import java.util.stream.Collectors;

public class TakaroRequestHandler {
    private final TakaroPlugin plugin;
    private final HytaleApiClient hytaleApi;
    private final Gson gson = new Gson();

    public TakaroRequestHandler(TakaroPlugin plugin, HytaleApiClient hytaleApi) {
        this.plugin = plugin;
        this.hytaleApi = hytaleApi;
    }

    public void handleRequest(dev.takaro.hytale.websocket.TakaroWebSocket sourceWebSocket, String requestId, String action, JsonObject payload) {
        Object responsePayload;

        try {
            switch (action) {
                case "testReachability":
                    responsePayload = handleTestReachability();
                    break;
                case "getPlayers":
                    responsePayload = handleGetPlayers();
                    break;
                case "getPlayer":
                    responsePayload = handleGetPlayer(payload);
                    break;
                case "getServerInfo":
                    responsePayload = handleGetServerInfo();
                    break;
                case "sendMessage":
                    responsePayload = handleSendMessage(payload);
                    break;
                case "setPlayerNameColor":
                    responsePayload = handleSetPlayerNameColor(payload);
                    break;
                case "executeCommand":
                case "executeConsoleCommand":
                    responsePayload = handleExecuteCommand(payload);
                    break;
                case "giveItem":
                    responsePayload = handleGiveItem(payload);
                    break;
                case "kickPlayer":
                    responsePayload = handleKickPlayer(payload);
                    break;
                case "banPlayer":
                    responsePayload = handleBanPlayer(payload);
                    break;
                case "unbanPlayer":
                    responsePayload = handleUnbanPlayer(payload);
                    break;
                case "getPlayerLocation":
                    responsePayload = handleGetPlayerLocation(payload);
                    break;
                case "teleportPlayer":
                    responsePayload = handleTeleportPlayer(payload);
                    break;
                case "teleportPlayerToPlayer":
                    responsePayload = handleTeleportPlayerToPlayer(payload);
                    break;
                case "listCommands":
                    responsePayload = handleListCommands();
                    break;
                case "getAvailableActions":
                    responsePayload = handleGetAvailableActions();
                    break;
                case "help":
                    responsePayload = buildHelpResponse();
                    break;
                case "getPlayerInventory":
                    responsePayload = handleGetPlayerInventory(payload);
                    break;
                case "listItems":
                    responsePayload = handleListItems();
                    break;
                case "getPlayerBedLocation":
                case "getPlayerBeds":
                    responsePayload = handleGetPlayerBedLocation(payload);
                    break;
                case "listBans":
                case "listEntities":
                case "listLocations":
                    // Not implemented yet
                    responsePayload = new Object[0];
                    break;
                default:
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log("Unknown action: " + action);
                    Map<String, String> error = new HashMap<>();
                    error.put("error", "Unknown action: " + action);
                    responsePayload = error;
            }
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling " + action + ": " + e.getMessage());
            e.printStackTrace();
            Map<String, String> error = new HashMap<>();
            error.put("error", e.getMessage());
            responsePayload = error;
        }

        // Send response back to the WebSocket that sent the request
        sourceWebSocket.sendResponse(requestId, responsePayload);
    }

    private Object handleTestReachability() {
        Map<String, Object> result = new HashMap<>();
        result.put("connectable", true);
        result.put("reason", null);
        return result;
    }

    private Object handleGetPlayers() {
        plugin.getLogger().at(java.util.logging.Level.FINE).log("Getting players list");

        try {
            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Universe is null");
                return new Object[0];
            }

            List<PlayerRef> players = universe.getPlayers();

            List<Map<String, Object>> playerList = players.stream().map(player -> {
                Map<String, Object> playerData = new HashMap<>();
                String uuid = player.getUuid().toString();
                playerData.put("name", player.getUsername());
                playerData.put("gameId", uuid);
                playerData.put("platformId", "hytale:" + uuid);

                // Extract real IP from player connection
                String ipAddress = "127.0.0.1";
                try {
                    io.netty.channel.Channel channel = player.getPacketHandler().getChannel();
                    java.net.SocketAddress remoteAddress = com.hypixel.hytale.server.core.io.netty.NettyUtil.getRemoteSocketAddress(channel);
                    if (remoteAddress instanceof java.net.InetSocketAddress) {
                        ipAddress = ((java.net.InetSocketAddress) remoteAddress).getAddress().getHostAddress();
                    }
                } catch (Exception e) {
                    // Keep default 127.0.0.1
                }
                playerData.put("ip", ipAddress);
                return playerData;
            }).collect(Collectors.toList());

            plugin.getLogger().at(java.util.logging.Level.FINE).log("Found " + playerList.size() + " players");
            return playerList;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting players: " + e.getMessage());
            e.printStackTrace();
            return new Object[0];
        }
    }

    private Object handleGetPlayer(JsonObject payload) {
        try {
            // Parse args to get player identifier
            String gameId = null;
            String playerName = null;

            if (payload.has("args")) {
                String argsString = payload.get("args").getAsString();
                JsonObject args = gson.fromJson(argsString, JsonObject.class);
                if (args.has("gameId")) {
                    gameId = args.get("gameId").getAsString();
                } else if (args.has("name")) {
                    playerName = args.get("name").getAsString();
                }
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("name")) {
                playerName = payload.get("name").getAsString();
            }

            if (gameId == null && playerName == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "No gameId or name provided");
                return error;
            }

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Universe is null");
                return error;
            }

            // Find player by gameId or name
            final String searchGameId = gameId;
            final String searchName = playerName;
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> {
                    if (searchGameId != null) {
                        return p.getUuid().toString().equals(searchGameId);
                    } else {
                        return p.getUsername().equalsIgnoreCase(searchName);
                    }
                })
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found");
                return error;
            }

            // Build player data
            String uuid = playerRef.getUuid().toString();
            Map<String, Object> playerData = new HashMap<>();
            playerData.put("name", playerRef.getUsername());
            playerData.put("gameId", uuid);
            playerData.put("platformId", "hytale:" + uuid);

            // Extract real IP from player connection
            String ipAddress = "127.0.0.1";
            try {
                io.netty.channel.Channel channel = playerRef.getPacketHandler().getChannel();
                java.net.SocketAddress remoteAddress = com.hypixel.hytale.server.core.io.netty.NettyUtil.getRemoteSocketAddress(channel);
                if (remoteAddress instanceof java.net.InetSocketAddress) {
                    ipAddress = ((java.net.InetSocketAddress) remoteAddress).getAddress().getHostAddress();
                }
            } catch (Exception e) {
                // Keep default 127.0.0.1
            }
            playerData.put("ip", ipAddress);

            return playerData;

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting player: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private Object handleGetServerInfo() {
        // TODO: Implement actual server info from Hytale API
        Map<String, Object> info = new HashMap<>();
        info.put("name", "Hytale Server");
        info.put("version", "1.0");
        return info;
    }

    private Object handleSendMessage(JsonObject payload) {
        try {
            // Parse args if it exists, otherwise try direct message field
            String message;
            String recipientGameId = null;

            if (payload.has("args")) {
                String argsString = payload.get("args").getAsString();
                JsonObject args = gson.fromJson(argsString, JsonObject.class);
                message = args.get("message").getAsString();

                // Check for opts.recipient.gameId for private messages
                if (args.has("opts")) {
                    JsonObject opts = args.getAsJsonObject("opts");
                    if (opts.has("recipient")) {
                        JsonObject recipient = opts.getAsJsonObject("recipient");
                        if (recipient.has("gameId")) {
                            recipientGameId = recipient.get("gameId").getAsString();
                        }
                    }
                }
            } else {
                message = payload.get("message").getAsString();
            }

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Universe is null");
                Map<String, Boolean> result = new HashMap<>();
                result.put("success", false);
                return result;
            }

            // Parse message from Takaro with clickable links
            Message msg = ChatFormatter.parseTakaroMessage(message);

            // If recipient is specified, send private message
            if (recipientGameId != null) {
                plugin.getLogger().at(java.util.logging.Level.FINE).log("Sending private message to player: " + recipientGameId);

                final String targetGameId = recipientGameId;
                PlayerRef targetPlayer = universe.getPlayers().stream()
                    .filter(p -> p.getUuid().toString().equals(targetGameId))
                    .findFirst()
                    .orElse(null);

                if (targetPlayer == null) {
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log("Target player not found: " + recipientGameId);
                    Map<String, Object> result = new HashMap<>();
                    result.put("success", false);
                    result.put("error", "Player not found: " + recipientGameId);
                    return result;
                }

                targetPlayer.sendMessage(msg);
                plugin.getLogger().at(java.util.logging.Level.FINE).log("Private message sent to player: " + targetPlayer.getUsername());

                Map<String, Boolean> result = new HashMap<>();
                result.put("success", true);
                return result;
            } else {
                // Send to all players (broadcast)
                plugin.getLogger().at(java.util.logging.Level.FINE).log("Sending message to all players: " + message);
                List<PlayerRef> players = universe.getPlayers();

                for (PlayerRef player : players) {
                    player.sendMessage(msg);
                }

                plugin.getLogger().at(java.util.logging.Level.FINE).log("Message sent to " + players.size() + " players");
                Map<String, Boolean> result = new HashMap<>();
                result.put("success", true);
                return result;
            }
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error sending message: " + e.getMessage());
            e.printStackTrace();
            Map<String, Boolean> result = new HashMap<>();
            result.put("success", false);
            return result;
        }
    }

    private Object handleSetPlayerNameColor(JsonObject payload) {
        try {
            // Parse args: {"uuid": "player-uuid", "color": "gold"}
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            String uuid = args.get("uuid").getAsString();
            String color = args.has("color") ? args.get("color").getAsString() : null;

            plugin.getLogger().at(java.util.logging.Level.INFO).log(
                "Setting name color for player " + uuid + ": " + color
            );

            // Update the cache
            plugin.setPlayerNameColor(uuid, color);

            Map<String, Boolean> result = new HashMap<>();
            result.put("success", true);
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error setting player name color: " + e.getMessage());
            e.printStackTrace();
            Map<String, Boolean> result = new HashMap<>();
            result.put("success", false);
            return result;
        }
    }

    private Object handleExecuteCommand(JsonObject payload) {
        try {
            // The payload structure is: {"args": "{\"command\":\"help\"}"}
            // We need to parse the args string as JSON
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);
            String command = args.get("command").getAsString().trim();

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Executing console command: '" + command + "'");

            // Check if it's a request for help/documentation
            if (command.equalsIgnoreCase("help") ||
                command.equalsIgnoreCase("commands") ||
                command.equalsIgnoreCase("getavailableactions") ||
                command.equalsIgnoreCase("takarohelp") ||
                command.equalsIgnoreCase("takaro")) {
                return buildHelpResponse();
            }

            // Check if it's a request to list Hytale commands
            if (command.equalsIgnoreCase("listcommands")) {
                return buildListCommandsResponse();
            }

            // Check for API action shortcuts typed as console commands
            if (command.equalsIgnoreCase("testReachability")) {
                return handleTestReachability();
            }
            if (command.equalsIgnoreCase("getPlayers")) {
                return handleGetPlayers();
            }
            if (command.equalsIgnoreCase("getServerInfo")) {
                return handleGetServerInfo();
            }
            if (command.equalsIgnoreCase("listItems")) {
                List<Map<String, Object>> items = (List<Map<String, Object>>) handleListItems();
                Map<String, Object> result = new HashMap<>();
                result.put("success", true);
                result.put("rawResult", "Found " + items.size() + " items. Use Takaro UI to view item list.");
                return result;
            }

            // sendMessage <message>
            if (command.toLowerCase().startsWith("sendmessage ")) {
                String message = command.substring("sendmessage ".length()).trim();
                JsonObject msgPayload = new JsonObject();
                JsonObject msgArgs = new JsonObject();
                msgArgs.addProperty("message", message);
                msgPayload.addProperty("args", gson.toJson(msgArgs));
                return handleSendMessage(msgPayload);
            }

            // getPlayerInventory <player>
            if (command.toLowerCase().startsWith("getplayerinventory ")) {
                String playerName = command.substring("getplayerinventory ".length()).trim();
                return getPlayerInventoryByName(playerName);
            }

            // getPlayerLocation <player>
            if (command.toLowerCase().startsWith("getplayerlocation ")) {
                String playerName = command.substring("getplayerlocation ".length()).trim();
                return getPlayerLocationByName(playerName);
            }

            // kickPlayer <player> [reason]
            if (command.toLowerCase().startsWith("kickplayer ")) {
                String[] parts = command.substring("kickplayer ".length()).split(" ", 2);
                String playerName = parts[0];
                String reason = parts.length > 1 ? parts[1] : "Kicked by admin";
                return kickPlayerByName(playerName, reason);
            }

            // banPlayer <player>
            if (command.toLowerCase().startsWith("banplayer ")) {
                String playerName = command.substring("banplayer ".length()).trim();
                return banPlayerByName(playerName);
            }

            // unbanPlayer <player>
            if (command.toLowerCase().startsWith("unbanplayer ")) {
                String playerName = command.substring("unbanplayer ".length()).trim();
                return unbanPlayerByName(playerName);
            }

            // Check if it's a request for player locations
            if (command.equalsIgnoreCase("playerlocations") ||
                command.equalsIgnoreCase("locations") ||
                command.equalsIgnoreCase("whereis") ||
                command.equalsIgnoreCase("players")) {
                return buildPlayerLocationsResponse();
            }

            // Check if it's a request for player bed locations
            if (command.toLowerCase().startsWith("beds ") ||
                command.toLowerCase().startsWith("playerbeds ")) {
                return handleBedsConsoleCommand(command);
            }

            // Check if it's a setcolor command
            if (command.toLowerCase().startsWith("setcolor ") ||
                command.toLowerCase().startsWith("namecolor ")) {
                return handleSetColorConsoleCommand(command);
            }

            // Check if it's a give command
            if (command.toLowerCase().startsWith("give ")) {
                plugin.getLogger().at(java.util.logging.Level.INFO).log("Matched give command, calling handler");
                return handleGiveConsoleCommand(command);
            }

            // Check if it's a teleport command
            if (command.toLowerCase().startsWith("teleportplayer ") ||
                command.toLowerCase().startsWith("tp ")) {
                return handleTeleportConsoleCommand(command);
            }

            // Check if it's a teleport player to player command
            if (command.toLowerCase().startsWith("teleportplayertoplayer ") ||
                command.toLowerCase().startsWith("tpp ")) {
                return handleTeleportPlayerToPlayerConsoleCommand(command);
            }

            // Check if it's a shutdown/stop command
            if (command.equalsIgnoreCase("shutdown") ||
                command.equalsIgnoreCase("stop")) {
                // Schedule shutdown after a delay to allow response to be sent
                new Thread(() -> {
                    try {
                        Thread.sleep(1000); // Wait 1 second to send response
                        HytaleServer.get().getCommandManager().handleCommand(ConsoleSender.INSTANCE, command).join();
                    } catch (Exception e) {
                        plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error executing delayed shutdown: " + e.getMessage());
                    }
                }).start();

                Map<String, Object> result = new HashMap<>();
                result.put("success", true);
                result.put("rawResult", "Server shutdown initiated");
                return result;
            }

            // Subscribe to logger to capture all console output
            CopyOnWriteArrayList<LogRecord> logCapture = new CopyOnWriteArrayList<>();
            HytaleLoggerBackend.subscribe(logCapture);

            try {
                // Execute command
                HytaleServer.get().getCommandManager().handleCommand(ConsoleSender.INSTANCE, command).join();

                // Give async messages time to arrive
                Thread.sleep(500);
            } finally {
                // Always unsubscribe
                HytaleLoggerBackend.unsubscribe(logCapture);
            }

            // Extract messages from captured logs
            StringBuilder output = new StringBuilder();
            for (LogRecord record : logCapture) {
                String message = record.getMessage();
                if (message != null && !message.isEmpty()) {
                    output.append(message).append("\n");
                }
            }

            String outputStr = output.toString().trim();
            plugin.getLogger().at(java.util.logging.Level.FINE).log("Captured " + logCapture.size() + " log records");

            if (outputStr.isEmpty()) {
                outputStr = "Command executed (no output)";
            } else {
                plugin.getLogger().at(java.util.logging.Level.FINE).log("Output preview: " + outputStr.substring(0, Math.min(100, outputStr.length())));
            }

            Map<String, Object> result = new HashMap<>();
            result.put("success", true);
            result.put("rawResult", outputStr);

            plugin.getLogger().at(java.util.logging.Level.FINE).log("Command executed: " + command + " | Output length: " + outputStr.length());
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error executing command: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Object handleGiveItem(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (args.has("player") && args.get("player").isJsonObject() && args.getAsJsonObject("player").has("gameId")) {
                gameId = args.getAsJsonObject("player").get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "No gameId or playerId provided");
                return result;
            }

            String itemId = args.get("item").getAsString();
            int amount = args.has("amount") ? args.get("amount").getAsInt() : 1;

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Giving item " + itemId + " x" + amount + " to player " + gameId);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Universe is null");
                return result;
            }

            // Support both UUID and player name
            PlayerRef playerRef;
            try {
                UUID playerUuid = UUID.fromString(gameId);
                playerRef = universe.getPlayers().stream()
                    .filter(p -> p.getUuid().equals(playerUuid))
                    .findFirst()
                    .orElse(null);
            } catch (IllegalArgumentException e) {
                // Not a UUID, try as player name
                final String playerName = gameId;
                playerRef = universe.getPlayers().stream()
                    .filter(p -> p.getUsername().equalsIgnoreCase(playerName))
                    .findFirst()
                    .orElse(null);
            }

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Player not found");
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Player not in world");
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            CompletableFuture<Boolean> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Player playerComponent = store.getComponent(ref, Player.getComponentType());
                    if (playerComponent == null) {
                        future.complete(false);
                        return;
                    }

                    Item item = Item.getAssetMap().getAsset(itemId);
                    if (item == null) {
                        plugin.getLogger().at(java.util.logging.Level.WARNING).log("Item not found: " + itemId);
                        future.complete(false);
                        return;
                    }

                    ItemStackTransaction transaction = playerComponent.getInventory()
                        .getCombinedHotbarFirst()
                        .addItemStack(new ItemStack(item.getId(), amount, null));

                    ItemStack remainder = transaction.getRemainder();
                    future.complete(remainder == null || remainder.isEmpty());
                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error giving item: " + e.getMessage());
                    e.printStackTrace();
                    future.complete(false);
                }
            });

            boolean success = future.get(5, TimeUnit.SECONDS);

            Map<String, Object> result = new HashMap<>();
            result.put("success", success);
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Give item result: " + success);
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling giveItem: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("error", e.getMessage());
            return result;
        }
    }

    private Object handleKickPlayer(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (args.has("player") && args.get("player").isJsonObject() && args.getAsJsonObject("player").has("gameId")) {
                gameId = args.getAsJsonObject("player").get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "No gameId or playerId provided");
                return result;
            }

            String reason = args.has("reason") ? args.get("reason").getAsString() : "You were kicked.";

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Kicking player: " + gameId);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Universe is null");
                return result;
            }

            UUID playerUuid = UUID.fromString(gameId);
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(playerUuid))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Player not found");
                return result;
            }

            playerRef.getPacketHandler().disconnect(reason);

            Map<String, Object> result = new HashMap<>();
            result.put("success", true);
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Player kicked: " + gameId);
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error kicking player: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("error", e.getMessage());
            return result;
        }
    }

    private Object handleBanPlayer(JsonObject payload) {
        // TODO: Implement player ban
        plugin.getLogger().at(java.util.logging.Level.INFO).log("Banning player: " + payload.toString());
        Map<String, Boolean> result = new HashMap<>();
        result.put("success", true);
        return result;
    }

    private Object handleUnbanPlayer(JsonObject payload) {
        // TODO: Implement player unban
        plugin.getLogger().at(java.util.logging.Level.INFO).log("Unbanning player: " + payload.toString());
        Map<String, Boolean> result = new HashMap<>();
        result.put("success", true);
        return result;
    }

    private Object handleGetPlayerLocation(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("x", 0);
                result.put("y", 0);
                result.put("z", 0);
                return result;
            }

            plugin.getLogger().at(java.util.logging.Level.FINE).log("Getting player location: " + gameId);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("x", 0);
                result.put("y", 0);
                result.put("z", 0);
                return result;
            }

            UUID playerUuid = UUID.fromString(gameId);
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(playerUuid))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("x", 0);
                result.put("y", 0);
                result.put("z", 0);
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("x", 0);
                result.put("y", 0);
                result.put("z", 0);
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            CompletableFuture<Map<String, Object>> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    TransformComponent transform = store.getComponent(ref, TransformComponent.getComponentType());
                    if (transform == null) {
                        Map<String, Object> result = new HashMap<>();
                        result.put("x", 0);
                        result.put("y", 0);
                        result.put("z", 0);
                        future.complete(result);
                        return;
                    }

                    Vector3d position = transform.getPosition();
                    Map<String, Object> result = new HashMap<>();
                    result.put("x", position.getX());
                    result.put("y", position.getY());
                    result.put("z", position.getZ());
                    future.complete(result);
                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting position: " + e.getMessage());
                    e.printStackTrace();
                    Map<String, Object> result = new HashMap<>();
                    result.put("x", 0);
                    result.put("y", 0);
                    result.put("z", 0);
                    future.complete(result);
                }
            });

            Map<String, Object> result = future.get(5, TimeUnit.SECONDS);
            plugin.getLogger().at(java.util.logging.Level.FINE).log("Player location: " + result.get("x") + "," + result.get("y") + "," + result.get("z"));
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling getPlayerLocation: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("x", 0);
            result.put("y", 0);
            result.put("z", 0);
            return result;
        }
    }

    private Object handleTeleportPlayerToPlayer(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String sourceGameId;
            if (args.has("gameId")) {
                sourceGameId = args.get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                sourceGameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                sourceGameId = payload.get("gameId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "No gameId or playerId provided for source player");
                return result;
            }

            // Get target gameId from args
            String targetGameId;
            if (args.has("targetGameId")) {
                targetGameId = args.get("targetGameId").getAsString();
            } else if (args.has("targetPlayerId")) {
                targetGameId = args.get("targetPlayerId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "No targetGameId or targetPlayerId provided");
                return result;
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Teleporting player " + sourceGameId + " to player " + targetGameId);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Universe is null");
                return result;
            }

            UUID sourceUuid = UUID.fromString(sourceGameId);
            UUID targetUuid = UUID.fromString(targetGameId);

            PlayerRef sourcePlayer = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(sourceUuid))
                .findFirst()
                .orElse(null);

            PlayerRef targetPlayer = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(targetUuid))
                .findFirst()
                .orElse(null);

            if (sourcePlayer == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Source player not found");
                return result;
            }

            if (targetPlayer == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Target player not found");
                return result;
            }

            Ref<EntityStore> sourceRef = sourcePlayer.getReference();
            Ref<EntityStore> targetRef = targetPlayer.getReference();

            if (sourceRef == null || !sourceRef.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Source player not in world");
                return result;
            }

            if (targetRef == null || !targetRef.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Target player not in world");
                return result;
            }

            Store<EntityStore> targetStore = targetRef.getStore();
            World targetWorld = targetStore.getExternalData().getWorld();

            // Get target player's position
            CompletableFuture<Vector3d> positionFuture = new CompletableFuture<>();

            targetWorld.execute(() -> {
                try {
                    TransformComponent targetTransform = targetStore.getComponent(targetRef, TransformComponent.getComponentType());
                    if (targetTransform == null) {
                        positionFuture.completeExceptionally(new Exception("Target transform not found"));
                        return;
                    }
                    positionFuture.complete(new Vector3d(targetTransform.getPosition()));
                } catch (Throwable e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting target position: " + e.getClass().getName() + ": " + e.getMessage());
                    e.printStackTrace();
                    positionFuture.completeExceptionally(e);
                }
            });

            Vector3d targetPosition;
            try {
                targetPosition = positionFuture.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Failed to get target position: " + e.getClass().getName() + ": " + e.getMessage());
                e.printStackTrace();
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Error getting target position: " + e.getMessage());
                return result;
            }

            // Now teleport source player to target position
            Store<EntityStore> sourceStore = sourceRef.getStore();
            World sourceWorld = sourceStore.getExternalData().getWorld();

            CompletableFuture<Boolean> teleportFuture = new CompletableFuture<>();

            sourceWorld.execute(() -> {
                try {
                    Vector3f rotation = new Vector3f(0, 0, 0);
                    Teleport teleport = new Teleport(targetWorld, targetPosition, rotation);
                    sourceStore.addComponent(sourceRef, Teleport.getComponentType(), teleport);
                    teleportFuture.complete(true);
                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error teleporting: " + e.getMessage());
                    e.printStackTrace();
                    teleportFuture.complete(false);
                }
            });

            boolean success = teleportFuture.get(5, TimeUnit.SECONDS);

            Map<String, Object> result = new HashMap<>();
            result.put("success", success);
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Teleport to player result: " + success);
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling teleportPlayerToPlayer: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("error", e.getMessage());
            return result;
        }
    }

    private Object handleTeleportPlayer(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (args.has("player") && args.get("player").isJsonObject() && args.getAsJsonObject("player").has("gameId")) {
                gameId = args.getAsJsonObject("player").get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "No gameId or playerId provided");
                return result;
            }

            double x = args.get("x").getAsDouble();
            double y = args.get("y").getAsDouble();
            double z = args.get("z").getAsDouble();

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Teleporting player " + gameId + " to " + x + "," + y + "," + z);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Universe is null");
                return result;
            }

            UUID playerUuid = UUID.fromString(gameId);
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(playerUuid))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Player not found");
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("error", "Player not in world");
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            CompletableFuture<Boolean> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Vector3d position = new Vector3d(x, y, z);
                    Vector3f rotation = new Vector3f(0, 0, 0);
                    Teleport teleport = new Teleport(world, position, rotation);
                    store.addComponent(ref, Teleport.getComponentType(), teleport);
                    future.complete(true);
                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error teleporting: " + e.getMessage());
                    e.printStackTrace();
                    future.complete(false);
                }
            });

            boolean success = future.get(5, TimeUnit.SECONDS);

            Map<String, Object> result = new HashMap<>();
            result.put("success", success);
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Teleport result: " + success);
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling teleportPlayer: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("error", e.getMessage());
            return result;
        }
    }

    private Object handleListCommands() {
        try {
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Fetching available commands from CommandManager");

            java.util.Map<String, com.hypixel.hytale.server.core.command.system.AbstractCommand> commands =
                HytaleServer.get().getCommandManager().getCommandRegistration();

            List<Map<String, Object>> commandList = new ArrayList<>();
            for (java.util.Map.Entry<String, com.hypixel.hytale.server.core.command.system.AbstractCommand> entry : commands.entrySet()) {
                Map<String, Object> commandInfo = new HashMap<>();
                commandInfo.put("name", entry.getKey());
                commandInfo.put("description", entry.getValue().getDescription());
                commandInfo.put("aliases", entry.getValue().getAliases());
                commandList.add(commandInfo);
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Returning " + commandList.size() + " commands");
            return commandList;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error listing commands: " + e.getMessage());
            e.printStackTrace();
            return new Object[0];
        }
    }

    private Map<String, Object> buildListCommandsResponse() {
        try {
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Building formatted command list for console");

            java.util.Map<String, com.hypixel.hytale.server.core.command.system.AbstractCommand> commands =
                HytaleServer.get().getCommandManager().getCommandRegistration();

            StringBuilder output = new StringBuilder();
            output.append("=== AVAILABLE HYTALE SERVER COMMANDS ===\n\n");
            output.append(String.format("Total commands: %d\n\n", commands.size()));

            // Sort commands alphabetically
            List<String> sortedCommands = new ArrayList<>(commands.keySet());
            Collections.sort(sortedCommands);

            for (String commandName : sortedCommands) {
                com.hypixel.hytale.server.core.command.system.AbstractCommand cmd = commands.get(commandName);
                output.append(String.format("/%s", commandName));

                Set<String> aliases = cmd.getAliases();
                if (aliases != null && !aliases.isEmpty()) {
                    output.append(" (aliases: ");
                    output.append(String.join(", ", aliases));
                    output.append(")");
                }

                String description = cmd.getDescription();
                if (description != null && !description.isEmpty()) {
                    output.append("\n  ");
                    output.append(description);
                }

                output.append("\n\n");
            }

            output.append("For Takaro custom commands, type: help\n");

            Map<String, Object> result = new HashMap<>();
            result.put("success", true);
            result.put("rawResult", output.toString());
            return result;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error building command list: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Object handleGetPlayerInventory(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("No gameId or playerId provided");
                return new Object[0];
            }

            plugin.getLogger().at(java.util.logging.Level.FINE).log("Getting player inventory for gameId: " + gameId);

        com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

        PlayerRef playerRef = null;
        for (PlayerRef ref : universe.getPlayers()) {
            if (ref.getUuid().toString().equals(gameId)) {
                playerRef = ref;
                break;
            }
        }

        if (playerRef == null) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player not found: " + gameId);
            return new Object[0];
        }

        Ref<EntityStore> ref = playerRef.getReference();
        if (ref == null || !ref.isValid()) {
            plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player reference not valid: " + gameId);
            return new Object[0];
        }

        Store<EntityStore> store = ref.getStore();
        World world = store.getExternalData().getWorld();

        // Access player inventory on the world thread
        List<Map<String, Object>> inventoryItems = new ArrayList<>();
        CompletableFuture<Void> future = new CompletableFuture<>();

        world.execute(() -> {
            try {
                Player player = store.getComponent(ref, Player.getComponentType());
                if (player == null) {
                    plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player component not found: " + gameId);
                    future.complete(null);
                    return;
                }

                // Get combined inventory (hotbar, storage, armor, utility, backpack)
                com.hypixel.hytale.server.core.inventory.Inventory inventory = player.getInventory();
                com.hypixel.hytale.server.core.inventory.container.CombinedItemContainer combined = inventory.getCombinedEverything();

                // Iterate through all inventory slots
                for (short i = 0; i < combined.getCapacity(); i++) {
                    ItemStack itemStack = combined.getItemStack(i);

                    // Skip empty slots
                    if (ItemStack.isEmpty(itemStack)) {
                        continue;
                    }

                    // Get item details
                    String itemId = itemStack.getItemId();
                    int quantity = itemStack.getQuantity();

                    // Get friendly name for the item
                    String friendlyName = itemId;
                    try {
                        Item item = Item.getAssetMap().getAssetMap().get(itemId);
                        if (item != null) {
                            String translationKey = item.getTranslationKey();
                            if (translationKey != null) {
                                String i18n = com.hypixel.hytale.server.core.modules.i18n.I18nModule.get().getMessage("en-US", translationKey);
                                if (i18n != null && !i18n.isEmpty()) {
                                    friendlyName = i18n;
                                }
                            }
                        }
                    } catch (Exception e) {
                        // Fall back to code if name lookup fails
                    }

                    // Create inventory item entry
                    Map<String, Object> inventoryItem = new HashMap<>();
                    inventoryItem.put("code", itemId);
                    inventoryItem.put("name", friendlyName);
                    inventoryItem.put("amount", quantity);

                    inventoryItems.add(inventoryItem);
                }

                plugin.getLogger().at(java.util.logging.Level.FINE).log("Found " + inventoryItems.size() + " items in player inventory");
                future.complete(null);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error reading inventory: " + e.getMessage());
                e.printStackTrace();
                future.completeExceptionally(e);
            }
        });

            // Wait for world thread to complete
            try {
                future.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Timeout waiting for inventory: " + e.getMessage());
            }

            return inventoryItems.toArray(new Object[0]);
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error parsing getPlayerInventory payload: " + e.getMessage());
            e.printStackTrace();
            return new Object[0];
        }
    }

    private Object handleGetPlayerBedLocation(JsonObject payload) {
        try {
            String argsString = payload.get("args").getAsString();
            JsonObject args = gson.fromJson(argsString, JsonObject.class);

            // Try to get gameId from args first, then fall back to top-level payload
            String gameId;
            if (args.has("gameId")) {
                gameId = args.get("gameId").getAsString();
            } else if (payload.has("playerId")) {
                gameId = payload.get("playerId").getAsString();
            } else if (payload.has("gameId")) {
                gameId = payload.get("gameId").getAsString();
            } else {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("No gameId or playerId provided");
                return new Object[0];
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Getting bed locations for player: " + gameId);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Universe is null");
                return new Object[0];
            }

            UUID playerUuid = UUID.fromString(gameId);
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUuid().equals(playerUuid))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player not found: " + gameId);
                return new Object[0];
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player reference not valid: " + gameId);
                return new Object[0];
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            // Access player bed locations on the world thread
            List<Map<String, Object>> bedLocations = new ArrayList<>();
            CompletableFuture<Void> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Player player = store.getComponent(ref, Player.getComponentType());
                    if (player == null) {
                        plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player component not found: " + gameId);
                        future.complete(null);
                        return;
                    }

                    // Get player configuration data
                    Object playerConfigData = player.getPlayerConfigData();
                    if (playerConfigData == null) {
                        plugin.getLogger().at(java.util.logging.Level.WARNING).log("PlayerConfigData is null");
                        future.complete(null);
                        return;
                    }

                    // Use reflection to access PlayerConfigData methods
                    try {
                        // Get per-world data
                        java.lang.reflect.Method getPerWorldDataMethod =
                            playerConfigData.getClass().getMethod("getPerWorldData", String.class);
                        Object playerWorldData = getPerWorldDataMethod.invoke(playerConfigData, world.getName());

                        if (playerWorldData == null) {
                            plugin.getLogger().at(java.util.logging.Level.INFO).log("No world data for " + world.getName());
                            future.complete(null);
                            return;
                        }

                        // Get respawn points array
                        java.lang.reflect.Method getRespawnPointsMethod =
                            playerWorldData.getClass().getMethod("getRespawnPoints");
                        Object[] respawnPoints = (Object[]) getRespawnPointsMethod.invoke(playerWorldData);

                        if (respawnPoints == null || respawnPoints.length == 0) {
                            plugin.getLogger().at(java.util.logging.Level.INFO).log("No respawn points found for player");
                            future.complete(null);
                            return;
                        }

                        // Extract bed information from each respawn point
                        for (Object respawnPoint : respawnPoints) {
                            try {
                                // Get block position (Vector3i)
                                java.lang.reflect.Method getBlockPositionMethod =
                                    respawnPoint.getClass().getMethod("getBlockPosition");
                                Object blockPosition = getBlockPositionMethod.invoke(respawnPoint);

                                // Get respawn position (Vector3d)
                                java.lang.reflect.Method getRespawnPositionMethod =
                                    respawnPoint.getClass().getMethod("getRespawnPosition");
                                Object respawnPosition = getRespawnPositionMethod.invoke(respawnPoint);

                                // Get bed name (String)
                                java.lang.reflect.Method getNameMethod =
                                    respawnPoint.getClass().getMethod("getName");
                                String bedName = (String) getNameMethod.invoke(respawnPoint);

                                // Extract coordinates from Vector3i (block position)
                                java.lang.reflect.Method getXMethod = blockPosition.getClass().getMethod("getX");
                                java.lang.reflect.Method getYMethod = blockPosition.getClass().getMethod("getY");
                                java.lang.reflect.Method getZMethod = blockPosition.getClass().getMethod("getZ");

                                int blockX = (int) getXMethod.invoke(blockPosition);
                                int blockY = (int) getYMethod.invoke(blockPosition);
                                int blockZ = (int) getZMethod.invoke(blockPosition);

                                // Extract coordinates from Vector3d (spawn position)
                                java.lang.reflect.Method getXDoubleMethod = respawnPosition.getClass().getMethod("getX");
                                java.lang.reflect.Method getYDoubleMethod = respawnPosition.getClass().getMethod("getY");
                                java.lang.reflect.Method getZDoubleMethod = respawnPosition.getClass().getMethod("getZ");

                                double spawnX = (double) getXDoubleMethod.invoke(respawnPosition);
                                double spawnY = (double) getYDoubleMethod.invoke(respawnPosition);
                                double spawnZ = (double) getZDoubleMethod.invoke(respawnPosition);

                                // Create bed location entry
                                Map<String, Object> bedLocation = new HashMap<>();
                                bedLocation.put("name", bedName != null ? bedName : "Unnamed Bed");
                                bedLocation.put("world", world.getName());
                                bedLocation.put("blockX", blockX);
                                bedLocation.put("blockY", blockY);
                                bedLocation.put("blockZ", blockZ);
                                bedLocation.put("spawnX", spawnX);
                                bedLocation.put("spawnY", spawnY);
                                bedLocation.put("spawnZ", spawnZ);

                                bedLocations.add(bedLocation);

                                plugin.getLogger().at(java.util.logging.Level.INFO).log(
                                    "Found bed: " + bedName + " at block(" + blockX + "," + blockY + "," + blockZ +
                                    ") spawn(" + spawnX + "," + spawnY + "," + spawnZ + ")"
                                );

                            } catch (Exception bedEx) {
                                plugin.getLogger().at(java.util.logging.Level.WARNING).log(
                                    "Error processing respawn point: " + bedEx.getMessage()
                                );
                            }
                        }

                        plugin.getLogger().at(java.util.logging.Level.INFO).log(
                            "Found " + bedLocations.size() + " bed locations for player"
                        );
                        future.complete(null);

                    } catch (Exception reflectionEx) {
                        plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                            "Error using reflection to access bed data: " + reflectionEx.getMessage()
                        );
                        reflectionEx.printStackTrace();
                        future.completeExceptionally(reflectionEx);
                    }

                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                        "Error getting bed locations: " + e.getMessage()
                    );
                    e.printStackTrace();
                    future.completeExceptionally(e);
                }
            });

            // Wait for world thread to complete
            try {
                future.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                    "Timeout waiting for bed locations: " + e.getMessage()
                );
            }

            return bedLocations.toArray(new Object[0]);

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                "Error parsing getPlayerBedLocation payload: " + e.getMessage()
            );
            e.printStackTrace();
            return new Object[0];
        }
    }

    private Object handleListItems() {
        try {
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Fetching all items from AssetMap");

            // Get the DefaultAssetMap
            com.hypixel.hytale.assetstore.map.DefaultAssetMap<String, Item> assetMap = Item.getAssetMap();
            plugin.getLogger().at(java.util.logging.Level.INFO).log("AssetMap retrieved: " + (assetMap != null ? "not null" : "NULL"));

            if (assetMap == null) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("AssetMap is NULL!");
                return new Object[0];
            }

            // Get the Map from the AssetMap
            Map<String, Item> items = assetMap.getAssetMap();
            plugin.getLogger().at(java.util.logging.Level.INFO).log("Items map retrieved: " + (items != null ? "not null, size=" + items.size() : "NULL"));

            if (items == null || items.isEmpty()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Items map is null or empty! Items may not be loaded yet.");
                return new Object[0];
            }

            List<Map<String, String>> itemList = new ArrayList<>();

            for (Map.Entry<String, Item> entry : items.entrySet()) {
                String code = entry.getKey();
                if (code == null || code.isEmpty()) continue;

                Item item = entry.getValue();
                if (item == null) continue;

                try {
                    String friendlyName = code;
                    String translationKey = item.getTranslationKey();
                    if (translationKey != null) {
                        String i18n = com.hypixel.hytale.server.core.modules.i18n.I18nModule.get().getMessage("en-US", translationKey);
                        if (i18n != null && !i18n.isEmpty()) {
                            friendlyName = i18n;
                        }
                    }

                    Map<String, String> itemInfo = new HashMap<>();
                    itemInfo.put("code", code);
                    itemInfo.put("name", friendlyName);
                    itemList.add(itemInfo);
                } catch (Exception e) {
                    Map<String, String> itemInfo = new HashMap<>();
                    itemInfo.put("code", code);
                    itemInfo.put("name", code);
                    itemList.add(itemInfo);
                }
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Successfully returning " + itemList.size() + " items");
            return itemList;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error listing items: " + e.getMessage());
            e.printStackTrace();
            return new ArrayList<>();
        }
    }

    private Map<String, Object> handleGiveConsoleCommand(String command) {
        try {
            // Parse: give <player> <item> [amount]
            String[] parts = command.split("\\s+");

            if (parts.length < 3) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Usage: give <player> <item> [amount]\nExample: give Mad001 Wood_Oak_Trunk 10");
                return result;
            }

            final String playerName = parts[1];
            final String itemName = parts[2];
            final int amount;

            if (parts.length >= 4) {
                try {
                    amount = Integer.parseInt(parts[3]);
                } catch (NumberFormatException e) {
                    Map<String, Object> result = new HashMap<>();
                    result.put("success", false);
                    result.put("rawResult", "Invalid amount: " + parts[3] + "\nUsage: give <player> <item> [amount]");
                    return result;
                }
            } else {
                amount = 1;
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Console give command: " + playerName + " " + itemName + " x" + amount);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null");
                return result;
            }

            // Find player by name
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(playerName))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not found: " + playerName);
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not in world: " + playerName);
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            CompletableFuture<String> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Player playerComponent = store.getComponent(ref, Player.getComponentType());
                    if (playerComponent == null) {
                        future.complete("Player component not found");
                        return;
                    }

                    Item item = Item.getAssetMap().getAsset(itemName);
                    if (item == null) {
                        future.complete("Item not found: " + itemName);
                        return;
                    }

                    ItemStackTransaction transaction = playerComponent.getInventory()
                        .getCombinedHotbarFirst()
                        .addItemStack(new ItemStack(item.getId(), amount, null));

                    ItemStack remainder = transaction.getRemainder();
                    if (remainder != null && !remainder.isEmpty()) {
                        future.complete("Gave " + (amount - remainder.getQuantity()) + " " + itemName + " to " + playerName + " (inventory full, " + remainder.getQuantity() + " dropped)");
                    } else {
                        future.complete("Gave " + amount + " " + itemName + " to " + playerName);
                    }
                } catch (Exception e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error giving item: " + e.getMessage());
                    e.printStackTrace();
                    future.complete("Error: " + e.getMessage());
                }
            });

            String resultMessage = future.get(5, TimeUnit.SECONDS);

            Map<String, Object> result = new HashMap<>();
            result.put("success", !resultMessage.startsWith("Error") && !resultMessage.contains("not found"));
            result.put("rawResult", resultMessage);
            return result;

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling give command: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> handleTeleportConsoleCommand(String command) {
        try {
            // Parse: teleportPlayer <player> <x> <y> <z> or tp <player> <x> <y> <z>
            String[] parts = command.split("\\s+");

            if (parts.length < 5) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Usage: teleportPlayer <player> <x> <y> <z>\nExample: teleportPlayer Hennyy 100 64 200\nOr use: tp <player> <x> <y> <z>");
                return result;
            }

            final String playerName = parts[1];
            final double x, y, z;

            try {
                x = Double.parseDouble(parts[2]);
                y = Double.parseDouble(parts[3]);
                z = Double.parseDouble(parts[4]);
            } catch (NumberFormatException e) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Invalid coordinates. Must be numbers.\nUsage: teleportPlayer <player> <x> <y> <z>");
                return result;
            }

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Console teleport command: " + playerName + " to " + x + "," + y + "," + z);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null");
                return result;
            }

            // Find player by name
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(playerName))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not found: " + playerName);
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not in world: " + playerName);
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            CompletableFuture<String> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Vector3d position = new Vector3d(x, y, z);
                    Vector3f rotation = new Vector3f(0, 0, 0);
                    Teleport teleport = new Teleport(world, position, rotation);
                    store.addComponent(ref, Teleport.getComponentType(), teleport);
                    future.complete("Teleported " + playerName + " to " + x + ", " + y + ", " + z);
                } catch (Throwable e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error teleporting: " + e.getClass().getName() + ": " + e.getMessage());
                    e.printStackTrace();
                    future.completeExceptionally(e);
                }
            });

            String resultMessage;
            try {
                resultMessage = future.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Teleport future failed: " + e.getClass().getName() + ": " + e.getMessage());
                e.printStackTrace();
                resultMessage = "Error: " + e.getMessage();
            }

            Map<String, Object> result = new HashMap<>();
            result.put("success", !resultMessage.startsWith("Error"));
            result.put("rawResult", resultMessage);
            return result;

        } catch (Throwable e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling teleport command: " + e.getClass().getName() + ": " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> handleTeleportPlayerToPlayerConsoleCommand(String command) {
        try {
            // Parse: teleportPlayerToPlayer <player> <targetPlayer> or tpp <player> <targetPlayer>
            String[] parts = command.split("\\s+");

            if (parts.length < 3) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Usage: teleportPlayerToPlayer <player> <targetPlayer>\nExample: teleportPlayerToPlayer Hennyy Mad001\nOr use: tpp <player> <targetPlayer>");
                return result;
            }

            final String sourcePlayerName = parts[1];
            final String targetPlayerName = parts[2];

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Console teleport command: " + sourcePlayerName + " to " + targetPlayerName);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null");
                return result;
            }

            // Find source player by name
            PlayerRef sourcePlayer = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(sourcePlayerName))
                .findFirst()
                .orElse(null);

            if (sourcePlayer == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Source player not found: " + sourcePlayerName);
                return result;
            }

            // Find target player by name
            PlayerRef targetPlayer = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(targetPlayerName))
                .findFirst()
                .orElse(null);

            if (targetPlayer == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Target player not found: " + targetPlayerName);
                return result;
            }

            Ref<EntityStore> sourceRef = sourcePlayer.getReference();
            Ref<EntityStore> targetRef = targetPlayer.getReference();

            if (sourceRef == null || !sourceRef.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Source player not in world: " + sourcePlayerName);
                return result;
            }

            if (targetRef == null || !targetRef.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Target player not in world: " + targetPlayerName);
                return result;
            }

            Store<EntityStore> targetStore = targetRef.getStore();
            World targetWorld = targetStore.getExternalData().getWorld();

            // Get target player's position
            CompletableFuture<Vector3d> positionFuture = new CompletableFuture<>();

            targetWorld.execute(() -> {
                try {
                    TransformComponent targetTransform = targetStore.getComponent(targetRef, TransformComponent.getComponentType());
                    if (targetTransform == null) {
                        positionFuture.completeExceptionally(new Exception("Target transform not found"));
                        return;
                    }
                    positionFuture.complete(new Vector3d(targetTransform.getPosition()));
                } catch (Throwable e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting target position: " + e.getClass().getName() + ": " + e.getMessage());
                    e.printStackTrace();
                    positionFuture.completeExceptionally(e);
                }
            });

            Vector3d targetPosition;
            try {
                targetPosition = positionFuture.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Failed to get target position: " + e.getClass().getName() + ": " + e.getMessage());
                e.printStackTrace();
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Error getting target position: " + e.getMessage());
                return result;
            }

            // Now teleport source player to target position
            Store<EntityStore> sourceStore = sourceRef.getStore();
            World sourceWorld = sourceStore.getExternalData().getWorld();

            CompletableFuture<String> teleportFuture = new CompletableFuture<>();

            sourceWorld.execute(() -> {
                try {
                    Vector3f rotation = new Vector3f(0, 0, 0);
                    Teleport teleport = new Teleport(targetWorld, targetPosition, rotation);
                    sourceStore.addComponent(sourceRef, Teleport.getComponentType(), teleport);
                    teleportFuture.complete("Teleported " + sourcePlayerName + " to " + targetPlayerName);
                } catch (Throwable e) {
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error teleporting: " + e.getClass().getName() + ": " + e.getMessage());
                    e.printStackTrace();
                    teleportFuture.completeExceptionally(e);
                }
            });

            String resultMessage;
            try {
                resultMessage = teleportFuture.get(5, TimeUnit.SECONDS);
            } catch (Exception e) {
                plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Teleport future failed: " + e.getClass().getName() + ": " + e.getMessage());
                e.printStackTrace();
                resultMessage = "Error: " + e.getMessage();
            }

            Map<String, Object> result = new HashMap<>();
            result.put("success", !resultMessage.startsWith("Error"));
            result.put("rawResult", resultMessage);
            return result;

        } catch (Throwable e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling teleport to player command: " + e.getClass().getName() + ": " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> buildPlayerLocationsResponse() {
        try {
            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null - cannot get player locations");
                return result;
            }

            List<PlayerRef> players = universe.getPlayers();

            if (players.isEmpty()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", true);
                result.put("rawResult", "No players online");
                return result;
            }

            StringBuilder output = new StringBuilder();
            output.append("=== ONLINE PLAYERS & LOCATIONS ===\n\n");
            output.append(String.format("Total players: %d\n\n", players.size()));

            for (PlayerRef player : players) {
                String playerName = player.getUsername();
                UUID playerUuid = player.getUuid();

                Ref<EntityStore> ref = player.getReference();
                if (ref == null || !ref.isValid()) {
                    output.append(String.format("%-20s - Not in world\n", playerName));
                    continue;
                }

                Store<EntityStore> store = ref.getStore();
                World world = store.getExternalData().getWorld();

                // Get location synchronously
                CompletableFuture<String> locationFuture = new CompletableFuture<>();

                world.execute(() -> {
                    try {
                        TransformComponent transform = store.getComponent(ref, TransformComponent.getComponentType());
                        if (transform == null) {
                            locationFuture.complete("Unknown location");
                            return;
                        }

                        Vector3d position = transform.getPosition();
                        String location = String.format("X: %.1f, Y: %.1f, Z: %.1f",
                            position.getX(), position.getY(), position.getZ());
                        locationFuture.complete(location);
                    } catch (Exception e) {
                        locationFuture.complete("Error: " + e.getMessage());
                    }
                });

                String location = locationFuture.get(2, TimeUnit.SECONDS);
                output.append(String.format("%-20s - %s\n", playerName, location));
            }

            Map<String, Object> result = new HashMap<>();
            result.put("success", true);
            result.put("rawResult", output.toString());
            return result;

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error building player locations: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error getting player locations: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> handleBedsConsoleCommand(String command) {
        try {
            // Parse: beds <player> or playerbeds <player>
            String[] parts = command.split("\\s+");

            if (parts.length < 2) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Usage: beds <player>\nExample: beds Hennyy\nOr: playerbeds Hennyy");
                return result;
            }

            final String playerName = parts[1];

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Console beds command for: " + playerName);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null");
                return result;
            }

            // Find player by name
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(playerName))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not found: " + playerName);
                return result;
            }

            Ref<EntityStore> ref = playerRef.getReference();
            if (ref == null || !ref.isValid()) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not in world: " + playerName);
                return result;
            }

            Store<EntityStore> store = ref.getStore();
            World world = store.getExternalData().getWorld();

            // Access player bed locations on the world thread
            CompletableFuture<String> future = new CompletableFuture<>();

            world.execute(() -> {
                try {
                    Player player = store.getComponent(ref, Player.getComponentType());
                    if (player == null) {
                        future.complete("ERROR: Player component not found for " + playerName);
                        return;
                    }

                    // Get player configuration data
                    Object playerConfigData = player.getPlayerConfigData();
                    if (playerConfigData == null) {
                        future.complete("No bed data available for " + playerName);
                        return;
                    }

                    // Use reflection to access PlayerConfigData methods
                    try {
                        // Get per-world data
                        java.lang.reflect.Method getPerWorldDataMethod =
                            playerConfigData.getClass().getMethod("getPerWorldData", String.class);
                        Object playerWorldData = getPerWorldDataMethod.invoke(playerConfigData, world.getName());

                        if (playerWorldData == null) {
                            future.complete(playerName + " has no beds in world: " + world.getName());
                            return;
                        }

                        // Get respawn points array
                        java.lang.reflect.Method getRespawnPointsMethod =
                            playerWorldData.getClass().getMethod("getRespawnPoints");
                        Object[] respawnPoints = (Object[]) getRespawnPointsMethod.invoke(playerWorldData);

                        if (respawnPoints == null || respawnPoints.length == 0) {
                            future.complete(playerName + " has no beds in world: " + world.getName());
                            return;
                        }

                        // Build output
                        StringBuilder output = new StringBuilder();
                        output.append("=== BED LOCATIONS FOR " + playerName.toUpperCase() + " ===\n\n");
                        output.append("World: " + world.getName() + "\n");
                        output.append("Total beds: " + respawnPoints.length + "\n\n");

                        // Extract bed information from each respawn point
                        for (int i = 0; i < respawnPoints.length; i++) {
                            Object respawnPoint = respawnPoints[i];
                            try {
                                // Get block position (Vector3i)
                                java.lang.reflect.Method getBlockPositionMethod =
                                    respawnPoint.getClass().getMethod("getBlockPosition");
                                Object blockPosition = getBlockPositionMethod.invoke(respawnPoint);

                                // Get respawn position (Vector3d)
                                java.lang.reflect.Method getRespawnPositionMethod =
                                    respawnPoint.getClass().getMethod("getRespawnPosition");
                                Object respawnPosition = getRespawnPositionMethod.invoke(respawnPoint);

                                // Get bed name (String)
                                java.lang.reflect.Method getNameMethod =
                                    respawnPoint.getClass().getMethod("getName");
                                String bedName = (String) getNameMethod.invoke(respawnPoint);

                                // Extract coordinates from Vector3i (block position)
                                java.lang.reflect.Method getXMethod = blockPosition.getClass().getMethod("getX");
                                java.lang.reflect.Method getYMethod = blockPosition.getClass().getMethod("getY");
                                java.lang.reflect.Method getZMethod = blockPosition.getClass().getMethod("getZ");

                                int blockX = (int) getXMethod.invoke(blockPosition);
                                int blockY = (int) getYMethod.invoke(blockPosition);
                                int blockZ = (int) getZMethod.invoke(blockPosition);

                                // Extract coordinates from Vector3d (spawn position)
                                java.lang.reflect.Method getXDoubleMethod = respawnPosition.getClass().getMethod("getX");
                                java.lang.reflect.Method getYDoubleMethod = respawnPosition.getClass().getMethod("getY");
                                java.lang.reflect.Method getZDoubleMethod = respawnPosition.getClass().getMethod("getZ");

                                double spawnX = (double) getXDoubleMethod.invoke(respawnPosition);
                                double spawnY = (double) getYDoubleMethod.invoke(respawnPosition);
                                double spawnZ = (double) getZDoubleMethod.invoke(respawnPosition);

                                // Format output
                                output.append("Bed #" + (i + 1) + ": " + (bedName != null ? bedName : "Unnamed Bed") + "\n");
                                output.append("  Block: X=" + blockX + ", Y=" + blockY + ", Z=" + blockZ + "\n");
                                output.append("  Spawn: X=" + String.format("%.1f", spawnX) +
                                    ", Y=" + String.format("%.1f", spawnY) +
                                    ", Z=" + String.format("%.1f", spawnZ) + "\n\n");

                            } catch (Exception bedEx) {
                                output.append("Bed #" + (i + 1) + ": Error reading bed data\n\n");
                                plugin.getLogger().at(java.util.logging.Level.WARNING).log(
                                    "Error processing respawn point: " + bedEx.getMessage()
                                );
                            }
                        }

                        future.complete(output.toString());

                    } catch (Exception reflectionEx) {
                        future.complete("ERROR: Could not read bed data: " + reflectionEx.getMessage());
                        plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                            "Error using reflection to access bed data: " + reflectionEx.getMessage()
                        );
                        reflectionEx.printStackTrace();
                    }

                } catch (Exception e) {
                    future.complete("ERROR: " + e.getMessage());
                    plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                        "Error getting bed locations: " + e.getMessage()
                    );
                    e.printStackTrace();
                }
            });

            // Wait for world thread to complete
            String resultMessage = future.get(5, TimeUnit.SECONDS);

            Map<String, Object> result = new HashMap<>();
            result.put("success", !resultMessage.startsWith("ERROR"));
            result.put("rawResult", resultMessage);
            return result;

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling beds command: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> handleSetColorConsoleCommand(String command) {
        try {
            // Parse: setcolor <player> <color> or namecolor <player> <color>
            String[] parts = command.split("\\s+");

            if (parts.length < 3) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Usage: setcolor <player> <color>\nExample: setcolor Mad001 gold\nOr: namecolor Mad001 ff0000");
                return result;
            }

            final String playerName = parts[1];
            final String color = parts[2];

            plugin.getLogger().at(java.util.logging.Level.FINE).log("Console setcolor command: " + playerName + " -> " + color);

            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Universe is null");
                return result;
            }

            // Find player by name
            PlayerRef playerRef = universe.getPlayers().stream()
                .filter(p -> p.getUsername().equalsIgnoreCase(playerName))
                .findFirst()
                .orElse(null);

            if (playerRef == null) {
                Map<String, Object> result = new HashMap<>();
                result.put("success", false);
                result.put("rawResult", "Player not found: " + playerName);
                return result;
            }

            String uuid = playerRef.getUuid().toString();

            // Update the cache
            plugin.setPlayerNameColor(uuid, color);

            Map<String, Object> result = new HashMap<>();
            result.put("success", true);
            result.put("rawResult", playerName + "'s chat name color set to: " + color);
            return result;

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling setcolor command: " + e.getMessage());
            e.printStackTrace();
            Map<String, Object> result = new HashMap<>();
            result.put("success", false);
            result.put("rawResult", "Error: " + e.getMessage());
            return result;
        }
    }

    private Map<String, Object> buildHelpResponse() {
        StringBuilder help = new StringBuilder();
        help.append("=== TAKARO API ACTIONS ===\n\n");

        help.append("1. testReachability\n");
        help.append("   Description: Test if the server is reachable\n");
        help.append("   Payload: {}\n\n");

        help.append("2. getPlayers\n");
        help.append("   Description: Get list of online players\n");
        help.append("   Payload: {}\n\n");

        help.append("3. getServerInfo\n");
        help.append("   Description: Get server information\n");
        help.append("   Payload: {}\n\n");

        help.append("4. sendMessage\n");
        help.append("   Description: Send message to all players (supports [red]text[-] or [ff0000]text[-])\n");
        help.append("   Payload: {\"args\": \"{\\\"message\\\":\\\"[red]Hello[-]\\\"}\"}}\n\n");

        help.append("5. executeCommand / executeConsoleCommand\n");
        help.append("   Description: Execute a console command\n");
        help.append("   Payload: {\"args\": \"{\\\"command\\\":\\\"who\\\"}\"}\n\n");

        help.append("6. giveItem\n");
        help.append("   Description: Give an item to a player\n");
        help.append("   Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\",\\\"item\\\":\\\"Wood_Oak_Trunk\\\",\\\"amount\\\":10}\"}\n\n");

        help.append("7. kickPlayer\n");
        help.append("   Description: Kick a player from the server\n");
        help.append("   Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\",\\\"reason\\\":\\\"kicked\\\"}\"}\n\n");

        help.append("8. banPlayer (not implemented)\n");
        help.append("   Description: Ban a player\n");
        help.append("   Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\"}\"}\n\n");

        help.append("9. unbanPlayer (not implemented)\n");
        help.append("   Description: Unban a player\n");
        help.append("   Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\"}\"}\n\n");

        help.append("10. getPlayerLocation\n");
        help.append("    Description: Get player's current position\n");
        help.append("    Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\"}\"}\n");
        help.append("    Returns: {\"x\": 123.45, \"y\": 67.89, \"z\": -234.56}\n\n");

        help.append("11. teleportPlayer\n");
        help.append("    Description: Teleport player to coordinates\n");
        help.append("    Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\",\\\"x\\\":100,\\\"y\\\":64,\\\"z\\\":200}\"}\n\n");

        help.append("12. teleportPlayerToPlayer\n");
        help.append("    Description: Teleport player to another player\n");
        help.append("    Payload: {\"args\": \"{\\\"gameId\\\":\\\"source-uuid\\\",\\\"targetGameId\\\":\\\"target-uuid\\\"}\"}\n\n");

        help.append("13. listCommands\n");
        help.append("    Description: Get all available Hytale server commands\n");
        help.append("    Payload: {}\n\n");

        help.append("14. getPlayerInventory (API limitations)\n");
        help.append("    Description: Get player's inventory (not readable via current Hytale API)\n");
        help.append("    Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\"}\"}\n");
        help.append("    Returns: []\n\n");

        help.append("15. getPlayerBedLocation / getPlayerBeds\n");
        help.append("    Description: Get all bed/respawn point locations for a player\n");
        help.append("    Payload: {\"args\": \"{\\\"gameId\\\":\\\"uuid\\\"}\"}\n");
        help.append("    Returns: [{\"name\":\"Bed Name\",\"world\":\"world\",\"blockX\":100,\"blockY\":64,\"blockZ\":200,\"spawnX\":100.5,\"spawnY\":64.5,\"spawnZ\":200.5}]\n\n");

        help.append("16. getAvailableActions / help\n");
        help.append("    Description: Get this list (API version)\n");
        help.append("    Payload: {}\n\n");

        help.append("=== CONSOLE COMMANDS ===\n");
        help.append("Type these in Takaro console:\n\n");
        help.append("HELP & INFO:\n");
        help.append("  - help (or: commands, getavailableactions, takarohelp, takaro)\n");
        help.append("    Shows this help menu\n\n");
        help.append("  - testReachability\n");
        help.append("    Test if server is reachable\n\n");
        help.append("  - getPlayers\n");
        help.append("    Get list of online players\n\n");
        help.append("  - getServerInfo\n");
        help.append("    Get server information\n\n");
        help.append("  - listItems\n");
        help.append("    Get list of all available items\n\n");
        help.append("  - listCommands\n");
        help.append("    Lists all available Hytale server commands\n\n");
        help.append("  - playerlocations (or: locations, whereis, players)\n");
        help.append("    Shows all online players and their coordinates\n\n");
        help.append("  - beds <player> (or: playerbeds)\n");
        help.append("    Shows all bed/respawn locations for a player\n");
        help.append("    Example: beds Hennyy\n\n");
        help.append("MESSAGING:\n");
        help.append("  - sendMessage <message>\n");
        help.append("    Send message to all players (supports [red]text[-] or [ff0000]text[-])\n");
        help.append("    Example: sendMessage Hello everyone!\n");
        help.append("    Example: sendMessage [red]Warning[-] Server restart in 5 minutes\n\n");
        help.append("PLAYER INFO:\n");
        help.append("  - getPlayerLocation <player>\n");
        help.append("    Get player's current coordinates\n");
        help.append("    Example: getPlayerLocation Hennyy\n\n");
        help.append("  - getPlayerInventory <player>\n");
        help.append("    Get player's inventory (API limitations - may return empty)\n");
        help.append("    Example: getPlayerInventory Mad001\n\n");
        help.append("PLAYER ACTIONS:\n");
        help.append("  - give <player> <item> [amount]\n");
        help.append("    Give items to a player\n");
        help.append("    Example: give Mad001 Wood_Oak_Trunk 10\n\n");
        help.append("  - teleportPlayer <player> <x> <y> <z> (or: tp)\n");
        help.append("    Teleport player to coordinates\n");
        help.append("    Example: tp Hennyy 100 64 200\n\n");
        help.append("  - teleportPlayerToPlayer <player> <targetPlayer> (or: tpp)\n");
        help.append("    Teleport player to another player\n");
        help.append("    Example: tpp Hennyy Mad001\n\n");
        help.append("  - setcolor <player> <color> (or: namecolor)\n");
        help.append("    Set a player's chat name color\n");
        help.append("    Example: setcolor Mad001 gold\n");
        help.append("    Example: namecolor Hennyy ff0000\n\n");
        help.append("MODERATION:\n");
        help.append("  - kickPlayer <player> [reason]\n");
        help.append("    Kick a player from the server\n");
        help.append("    Example: kickPlayer Hennyy\n");
        help.append("    Example: kickPlayer Mad001 Breaking rules\n\n");
        help.append("  - banPlayer <player>\n");
        help.append("    Ban a player (not implemented)\n");
        help.append("    Example: banPlayer Griefer123\n\n");
        help.append("  - unbanPlayer <player>\n");
        help.append("    Unban a player (not implemented)\n");
        help.append("    Example: unbanPlayer Griefer123\n\n");
        help.append("STANDARD HYTALE:\n");
        help.append("  - who, version, kick, etc. (all standard Hytale commands work)\n\n");

        Map<String, Object> result = new HashMap<>();
        result.put("success", true);
        result.put("rawResult", help.toString());
        return result;
    }

    private Object handleGetAvailableActions() {
        List<Map<String, Object>> actions = new ArrayList<>();

        // testReachability
        Map<String, Object> testReachability = new HashMap<>();
        testReachability.put("action", "testReachability");
        testReachability.put("description", "Test if the server is reachable");
        testReachability.put("payload", "{}");
        testReachability.put("returns", "{\"connectable\": true, \"reason\": null}");
        actions.add(testReachability);

        // getPlayers
        Map<String, Object> getPlayers = new HashMap<>();
        getPlayers.put("action", "getPlayers");
        getPlayers.put("description", "Get list of online players");
        getPlayers.put("payload", "{}");
        getPlayers.put("returns", "[{\"name\": \"PlayerName\", \"gameId\": \"uuid\", \"platformId\": \"hytale:uuid\", \"ip\": \"127.0.0.1\"}]");
        actions.add(getPlayers);

        // getPlayer
        Map<String, Object> getPlayer = new HashMap<>();
        getPlayer.put("action", "getPlayer");
        getPlayer.put("description", "Get information about a specific player by gameId or name");
        getPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        getPlayer.put("returns", "{\"name\": \"PlayerName\", \"gameId\": \"uuid\", \"platformId\": \"hytale:uuid\", \"ip\": \"127.0.0.1\"}");
        actions.add(getPlayer);

        // getServerInfo
        Map<String, Object> getServerInfo = new HashMap<>();
        getServerInfo.put("action", "getServerInfo");
        getServerInfo.put("description", "Get server information");
        getServerInfo.put("payload", "{}");
        getServerInfo.put("returns", "{\"name\": \"Hytale Server\", \"version\": \"1.0\"}");
        actions.add(getServerInfo);

        // sendMessage
        Map<String, Object> sendMessage = new HashMap<>();
        sendMessage.put("action", "sendMessage");
        sendMessage.put("description", "Send a message to all players (supports color codes: [red]text[-] or [ff0000]text[-])");
        sendMessage.put("payload", "{\"args\": \"{\\\"message\\\":\\\"[red]Hello[-] everyone!\\\"}\"}");
        sendMessage.put("returns", "{\"success\": true}");
        actions.add(sendMessage);

        // executeCommand / executeConsoleCommand
        Map<String, Object> executeCommand = new HashMap<>();
        executeCommand.put("action", "executeCommand / executeConsoleCommand");
        executeCommand.put("description", "Execute a console command and capture output");
        executeCommand.put("payload", "{\"args\": \"{\\\"command\\\":\\\"who\\\"}\"}");
        executeCommand.put("returns", "{\"success\": true, \"rawResult\": \"Console executed command: who\\ndefault (1): : PlayerName (PlayerName)\"}");
        actions.add(executeCommand);

        // giveItem
        Map<String, Object> giveItem = new HashMap<>();
        giveItem.put("action", "giveItem");
        giveItem.put("description", "Give an item to a player");
        giveItem.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\",\\\"item\\\":\\\"Wood_Deadwood_Decorative\\\",\\\"amount\\\":10}\"}");
        giveItem.put("returns", "{\"success\": true}");
        actions.add(giveItem);

        // kickPlayer
        Map<String, Object> kickPlayer = new HashMap<>();
        kickPlayer.put("action", "kickPlayer");
        kickPlayer.put("description", "Kick a player from the server");
        kickPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\",\\\"reason\\\":\\\"You were kicked\\\"}\"}");
        kickPlayer.put("returns", "{\"success\": true}");
        actions.add(kickPlayer);

        // banPlayer
        Map<String, Object> banPlayer = new HashMap<>();
        banPlayer.put("action", "banPlayer");
        banPlayer.put("description", "Ban a player (not implemented yet)");
        banPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        banPlayer.put("returns", "{\"success\": true}");
        actions.add(banPlayer);

        // unbanPlayer
        Map<String, Object> unbanPlayer = new HashMap<>();
        unbanPlayer.put("action", "unbanPlayer");
        unbanPlayer.put("description", "Unban a player (not implemented yet)");
        unbanPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        unbanPlayer.put("returns", "{\"success\": true}");
        actions.add(unbanPlayer);

        // getPlayerLocation
        Map<String, Object> getPlayerLocation = new HashMap<>();
        getPlayerLocation.put("action", "getPlayerLocation");
        getPlayerLocation.put("description", "Get a player's current position");
        getPlayerLocation.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        getPlayerLocation.put("returns", "{\"x\": 123.45, \"y\": 67.89, \"z\": -234.56}");
        actions.add(getPlayerLocation);

        // teleportPlayer
        Map<String, Object> teleportPlayer = new HashMap<>();
        teleportPlayer.put("action", "teleportPlayer");
        teleportPlayer.put("description", "Teleport a player to coordinates");
        teleportPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\",\\\"x\\\":100,\\\"y\\\":64,\\\"z\\\":200}\"}");
        teleportPlayer.put("returns", "{\"success\": true}");
        actions.add(teleportPlayer);

        // teleportPlayerToPlayer
        Map<String, Object> teleportPlayerToPlayer = new HashMap<>();
        teleportPlayerToPlayer.put("action", "teleportPlayerToPlayer");
        teleportPlayerToPlayer.put("description", "Teleport a player to another player");
        teleportPlayerToPlayer.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"source-uuid\\\",\\\"targetGameId\\\":\\\"target-uuid\\\"}\"}");
        teleportPlayerToPlayer.put("returns", "{\"success\": true}");
        actions.add(teleportPlayerToPlayer);

        // listCommands
        Map<String, Object> listCommands = new HashMap<>();
        listCommands.put("action", "listCommands");
        listCommands.put("description", "Get list of all available Hytale server commands");
        listCommands.put("payload", "{}");
        listCommands.put("returns", "[{\"name\": \"help\", \"description\": \"Shows help\", \"aliases\": [\"?\"]}]");
        actions.add(listCommands);

        // getPlayerInventory
        Map<String, Object> getPlayerInventory = new HashMap<>();
        getPlayerInventory.put("action", "getPlayerInventory");
        getPlayerInventory.put("description", "Get a player's inventory (Hytale API limitations prevent reading)");
        getPlayerInventory.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        getPlayerInventory.put("returns", "[]");
        actions.add(getPlayerInventory);

        // getPlayerBedLocation
        Map<String, Object> getPlayerBedLocation = new HashMap<>();
        getPlayerBedLocation.put("action", "getPlayerBedLocation");
        getPlayerBedLocation.put("description", "Get all bed/respawn point locations for a player");
        getPlayerBedLocation.put("payload", "{\"args\": \"{\\\"gameId\\\":\\\"player-uuid\\\"}\"}");
        getPlayerBedLocation.put("returns", "[{\"name\":\"Bed Name\",\"world\":\"world\",\"blockX\":100,\"blockY\":64,\"blockZ\":200,\"spawnX\":100.5,\"spawnY\":64.5,\"spawnZ\":200.5}]");
        actions.add(getPlayerBedLocation);

        // getAvailableActions / help
        Map<String, Object> help = new HashMap<>();
        help.put("action", "getAvailableActions / help");
        help.put("description", "Get this list of available actions");
        help.put("payload", "{}");
        help.put("returns", "[{\"action\": \"...\", \"description\": \"...\", \"payload\": \"...\", \"returns\": \"...\"}]");
        actions.add(help);

        plugin.getLogger().at(java.util.logging.Level.INFO).log("Returning " + actions.size() + " available actions");
        return actions;
    }

    // Helper methods for console command shortcuts

    private Object getPlayerInventoryByName(String playerName) {
        try {
            String gameId = getGameIdByName(playerName);
            if (gameId == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found: " + playerName);
                return error;
            }

            JsonObject payload = new JsonObject();
            JsonObject args = new JsonObject();
            args.addProperty("gameId", gameId);
            payload.addProperty("args", gson.toJson(args));
            return handleGetPlayerInventory(payload);
        } catch (Exception e) {
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private Object getPlayerLocationByName(String playerName) {
        try {
            String gameId = getGameIdByName(playerName);
            if (gameId == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found: " + playerName);
                return error;
            }

            JsonObject payload = new JsonObject();
            JsonObject args = new JsonObject();
            args.addProperty("gameId", gameId);
            payload.addProperty("args", gson.toJson(args));
            return handleGetPlayerLocation(payload);
        } catch (Exception e) {
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private Object kickPlayerByName(String playerName, String reason) {
        try {
            String gameId = getGameIdByName(playerName);
            if (gameId == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found: " + playerName);
                return error;
            }

            JsonObject payload = new JsonObject();
            JsonObject args = new JsonObject();
            args.addProperty("gameId", gameId);
            args.addProperty("reason", reason);
            payload.addProperty("args", gson.toJson(args));
            return handleKickPlayer(payload);
        } catch (Exception e) {
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private Object banPlayerByName(String playerName) {
        try {
            String gameId = getGameIdByName(playerName);
            if (gameId == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found: " + playerName);
                return error;
            }

            JsonObject payload = new JsonObject();
            JsonObject args = new JsonObject();
            args.addProperty("gameId", gameId);
            payload.addProperty("args", gson.toJson(args));
            return handleBanPlayer(payload);
        } catch (Exception e) {
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private Object unbanPlayerByName(String playerName) {
        try {
            String gameId = getGameIdByName(playerName);
            if (gameId == null) {
                Map<String, Object> error = new HashMap<>();
                error.put("success", false);
                error.put("error", "Player not found: " + playerName);
                return error;
            }

            JsonObject payload = new JsonObject();
            JsonObject args = new JsonObject();
            args.addProperty("gameId", gameId);
            payload.addProperty("args", gson.toJson(args));
            return handleUnbanPlayer(payload);
        } catch (Exception e) {
            Map<String, Object> error = new HashMap<>();
            error.put("success", false);
            error.put("error", e.getMessage());
            return error;
        }
    }

    private String getGameIdByName(String playerName) {
        try {
            com.hypixel.hytale.server.core.universe.Universe universe =
                com.hypixel.hytale.server.core.universe.Universe.get();

            if (universe == null) {
                return null;
            }

            List<PlayerRef> players = universe.getPlayers();
            for (PlayerRef player : players) {
                if (player.getUsername().equalsIgnoreCase(playerName)) {
                    return player.getUuid().toString();
                }
            }
            return null;
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error getting gameId by name: " + e.getMessage());
            return null;
        }
    }
}
