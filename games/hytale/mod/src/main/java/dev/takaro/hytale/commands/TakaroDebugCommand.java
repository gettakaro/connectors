package dev.takaro.hytale.commands;

import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.command.system.CommandContext;
import com.hypixel.hytale.server.core.command.system.basecommands.CommandBase;
import dev.takaro.hytale.TakaroPlugin;

import javax.annotation.Nonnull;
import java.lang.reflect.Method;

/**
 * Debug command to help discover Hytale server API
 * Usage: /takarodebug [info|server|events|ws]
 */
public class TakaroDebugCommand extends CommandBase {
    private final TakaroPlugin plugin;

    public TakaroDebugCommand(TakaroPlugin plugin) {
        super("takarodebug", "Debug Takaro integration and explore Hytale API");
        this.plugin = plugin;
    }

    @Override
    protected void executeSync(@Nonnull CommandContext context) {
        // Get input string and split into arguments
        String input = context.getInputString();
        String[] parts = input.trim().split("\\s+");

        // Skip first part which is the command name itself
        String[] args = new String[parts.length - 1];
        System.arraycopy(parts, 1, args, 0, args.length);

        if (args.length == 0) {
            sendHelp(context);
            return;
        }

        String subcommand = args[0].toLowerCase();

        switch (subcommand) {
            case "info":
                showInfo(context);
                break;
            case "server":
                showServerInfo(context);
                break;
            case "events":
                showEventInfo(context);
                break;
            case "ws":
                showWebSocketInfo(context);
                break;
            case "methods":
                showServerMethods(context);
                break;
            case "testlink":
                testClickableLink(context);
                break;
            default:
                sendHelp(context);
        }
    }

    private void sendHelp(CommandContext context) {
        context.sendMessage(Message.raw("§aTakaro Debug Commands:"));
        context.sendMessage(Message.raw("§e/takarodebug info §7- Show plugin info"));
        context.sendMessage(Message.raw("§e/takarodebug server §7- Show server info"));
        context.sendMessage(Message.raw("§e/takarodebug events §7- Show event registration status"));
        context.sendMessage(Message.raw("§e/takarodebug ws §7- Show WebSocket connection status"));
        context.sendMessage(Message.raw("§e/takarodebug methods §7- List server methods (console only)"));
        context.sendMessage(Message.raw("§e/takarodebug testlink §7- Test clickable links"));
    }

    private void showInfo(CommandContext context) {
        context.sendMessage(Message.raw("§a=== Takaro Integration Info ==="));
        context.sendMessage(Message.raw("§7Plugin Version: §e" + plugin.getVersion()));
        context.sendMessage(Message.raw("§7Takaro WS URL: §e" + plugin.getConfig().getWsUrl()));

        boolean hasTakaroToken = !plugin.getConfig().getIdentityToken().isEmpty();
        boolean hasHytaleToken = !plugin.getConfig().getHytaleApiToken().isEmpty();

        context.sendMessage(Message.raw("§7Identity Token: " + (hasTakaroToken ? "§aConfigured" : "§cMissing")));

        // Show Hytale API status (hidden feature - not documented yet)
        if (hasHytaleToken) {
            context.sendMessage(Message.raw("§7Hytale API: §aEnabled (hidden feature)"));
        }
    }

    private void showServerInfo(CommandContext context) {
        context.sendMessage(Message.raw("§a=== Server Info ==="));

        try {
            // Try to get server instance
            // Note: Actual method name may vary, will discover through testing
            // Object server = plugin.getServer();

            context.sendMessage(Message.raw("§7Plugin loaded: §aYes"));

            // TODO: Add actual player count once API is discovered
            // context.sendMessage(Message.raw("§7Online Players: §e" + server.getOnlinePlayers().size()));

            context.sendMessage(Message.raw("§eNote: Use /takarodebug methods in console to see available APIs"));
        } catch (Exception e) {
            context.sendMessage(Message.raw("§cError: " + e.getMessage()));
        }
    }

    private void showEventInfo(CommandContext context) {
        context.sendMessage(Message.raw("§a=== Event Registration ==="));
        context.sendMessage(Message.raw("§7Chat Listener: §eRegistration pending"));
        context.sendMessage(Message.raw("§7Player Listener: §eRegistration pending"));
        context.sendMessage(Message.raw("§eNote: Event registration will be implemented once API is tested"));
    }

    private void showWebSocketInfo(CommandContext context) {
        context.sendMessage(Message.raw("§a=== Takaro WebSocket ==="));

        if (plugin.getWebSocket() == null) {
            context.sendMessage(Message.raw("§cWebSocket: Not initialized"));
            return;
        }

        boolean isConnected = plugin.getWebSocket().isOpen();
        boolean isIdentified = plugin.getWebSocket().isIdentified();

        context.sendMessage(Message.raw("§7Connected: " + (isConnected ? "§aYes" : "§cNo")));
        context.sendMessage(Message.raw("§7Identified: " + (isIdentified ? "§aYes" : "§cNo")));

        if (!isConnected) {
            context.sendMessage(Message.raw("§eCheck config.properties for correct IDENTITY_TOKEN"));
        } else if (!isIdentified) {
            context.sendMessage(Message.raw("§eWaiting for Takaro authentication..."));
        } else {
            context.sendMessage(Message.raw("§a✓ Ready to send/receive Takaro events!"));
        }
    }

    private void showServerMethods(CommandContext context) {
        plugin.getLogger().at(java.util.logging.Level.INFO).log("=== Available Server Methods ===");

        try {
            // Reflect on the plugin to find server-related methods
            Class<?> pluginClass = plugin.getClass().getSuperclass(); // JavaPlugin

            plugin.getLogger().at(java.util.logging.Level.INFO).log("Plugin superclass: " + pluginClass.getName());

            for (Method method : pluginClass.getDeclaredMethods()) {
                String methodName = method.getName();
                if (methodName.contains("server") || methodName.contains("Server") ||
                    methodName.contains("event") || methodName.contains("Event") ||
                    methodName.contains("player") || methodName.contains("Player")) {

                    plugin.getLogger().at(java.util.logging.Level.INFO).log("  - " + method.getName() + "()");
                }
            }

            context.sendMessage(Message.raw("§aMethod list printed to console (check logs)"));

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error listing methods: " + e.getMessage());
            context.sendMessage(Message.raw("§cError: " + e.getMessage()));
        }
    }

    private void testClickableLink(CommandContext context) {
        // Test 1: Direct link creation (like InfoPanel does it)
        Message linkMsg1 = Message.raw("takaro.io").link("https://takaro.io").color("#00BFFF");
        context.sendMessage(Message.raw("Test 1 (direct): ").insert(linkMsg1));

        // Test 2: Using ChatFormatter.parseColoredMessage with plain URL
        Message parsed1 = dev.takaro.hytale.handlers.ChatFormatter.parseColoredMessage("Check out https://takaro.io for more info");
        context.sendMessage(Message.raw("Test 2 (plain URL): ").insert(parsed1));

        // Test 3: Using ChatFormatter with colored text containing URL
        Message parsed2 = dev.takaro.hytale.handlers.ChatFormatter.parseColoredMessage("[red]Visit https://takaro.io now![-]");
        context.sendMessage(Message.raw("Test 3 (colored URL): ").insert(parsed2));

        // Test 4: Multiple URLs
        Message parsed3 = dev.takaro.hytale.handlers.ChatFormatter.parseColoredMessage("Links: https://takaro.io and https://google.com");
        context.sendMessage(Message.raw("Test 4 (multiple): ").insert(parsed3));
    }
}
