package dev.takaro.hytale.config;

import java.io.*;
import java.util.Properties;

public class TakaroConfig {
    private final Properties properties;
    private final File configFile;

    public TakaroConfig(File configFile) {
        this.configFile = configFile;
        this.properties = new Properties();
        load();
    }

    /**
     * Re-read the properties file from disk.
     *
     * <p>Called before every identify attempt so an operator who corrects a stale
     * REGISTRATION_TOKEN does not have to restart the server.
     */
    public synchronized void reload() {
        try {
            if (configFile.isFile()) {
                Properties fresh = new Properties();
                try (FileInputStream fis = new FileInputStream(configFile)) {
                    fresh.load(fis);
                }
                properties.clear();
                properties.putAll(fresh);
            }
        } catch (IOException e) {
            // Keep the values already in memory rather than losing the connection over it.
        }
    }

    private void load() {
        if (!configFile.exists()) {
            createDefault();
        }

        try (FileInputStream fis = new FileInputStream(configFile)) {
            properties.load(fis);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    private void createDefault() {
        properties.setProperty("IDENTITY_TOKEN", "MyHytaleServer");
        properties.setProperty("REGISTRATION_TOKEN", "");
        properties.setProperty("COMMAND_PREFIX", "!");
        properties.setProperty("COMMAND_RESPONSE", "[cyan]Command[-] [green]{prefix}{command}[-]");
        properties.setProperty("TAKARO_DEBUG", "false");
        properties.setProperty("LOG_FORWARD_LEVEL", "INFO");
        properties.setProperty("LOG_FORWARD_MAX_PER_MIN", "120");
        properties.setProperty("EVENT_QUEUE_SIZE", "1000");
        properties.setProperty("CATALOG_INCLUDE_DEBUG", "false");
        properties.setProperty("COMMAND_SETTLE_MS", "0");

        // HytaleCharts integration defaults
        properties.setProperty("HYTALECHARTS_SECRET", "");
        properties.setProperty("HYTALECHARTS_DEBUG", "false");
        properties.setProperty("HYTALECHARTS_PROMO_ON_LOGIN", "false");
        properties.setProperty("HYTALECHARTS_PROMO_ENABLED", "false");
        properties.setProperty("HYTALECHARTS_PROMO_INTERVAL_MINUTES", "15");
        properties.setProperty("HYTALECHARTS_PROMO_PREFIX", "[hytalecharts.com] ");
        properties.setProperty("HYTALECHARTS_PROMO_MESSAGE", "Vote for our server on HytaleCharts!");
        properties.setProperty("HYTALECHARTS_PROMO_URL", "https://hytalecharts.com");

        try {
            configFile.getParentFile().mkdirs();
            try (FileWriter writer = new FileWriter(configFile)) {
                writer.write("# Takaro Integration Configuration\n");
                writer.write("# \n");
                writer.write("# Production Takaro Configuration:\n");
                writer.write("# IDENTITY_TOKEN: Choose a name for your server (e.g., MyHytaleServer, SurvivalServer, etc.)\n");
                writer.write("# REGISTRATION_TOKEN: Get this from Takaro dashboard (Settings → Game Servers → Add Server → Generic)\n");
                writer.write("# COMMAND_PREFIX: Prefix for Takaro commands (default: !). Change to ., -, or any custom prefix (do not use / - reserved by Hytale).\n");
                writer.write("# COMMAND_RESPONSE: Private message sent when command received. Use {prefix} and {command} placeholders. Supports color codes like [cyan]text[-].\n");
                writer.write("# \n");
                writer.write("IDENTITY_TOKEN=MyHytaleServer\n");
                writer.write("REGISTRATION_TOKEN=\n");
                writer.write("COMMAND_PREFIX=!\n");
                writer.write("COMMAND_RESPONSE=[cyan]Command[-] [green]{prefix}{command}[-]\n");
                writer.write("\n");
                writer.write("# Diagnostics:\n");
                writer.write("# TAKARO_DEBUG: log every WebSocket frame (direction, type, requestId, action, player identity fields) at INFO.\n");
                writer.write("# LOG_FORWARD_LEVEL: minimum server log level forwarded to Takaro (OFF disables forwarding).\n");
                writer.write("# LOG_FORWARD_MAX_PER_MIN: cap on forwarded log records per minute (0 = unlimited).\n");
                writer.write("# EVENT_QUEUE_SIZE: how many game events to hold while Takaro is unreachable.\n");
                writer.write("TAKARO_DEBUG=false\n");
                writer.write("LOG_FORWARD_LEVEL=INFO\n");
                writer.write("LOG_FORWARD_MAX_PER_MIN=120\n");
                writer.write("EVENT_QUEUE_SIZE=1000\n");
                writer.write("# CATALOG_INCLUDE_DEBUG: include Debug_/Test_/Dev_ entries in listItems and listEntities.\n");
                writer.write("CATALOG_INCLUDE_DEBUG=false\n");
                writer.write("# COMMAND_SETTLE_MS: extra time to keep collecting a console command's output\n");
                writer.write("# after it finished (0 = none; Hytale console commands are synchronous).\n");
                writer.write("COMMAND_SETTLE_MS=0\n");
                writer.write("\n");
                writer.write("# HytaleCharts Integration (third party, OPT-IN - all of it is off below):\n");
                writer.write("# Setting HYTALECHARTS_SECRET sends this server's player list (usernames and\n");
                writer.write("# UUIDs) to hytalecharts.com every 5 minutes. Leave it blank to send nothing.\n");
                writer.write("# HYTALECHARTS_SECRET: Get this from hytalecharts.com (generate heartbeat secret)\n");
                writer.write("# HYTALECHARTS_DEBUG: Enable debug logging (true/false)\n");
                writer.write("# HYTALECHARTS_PROMO_ON_LOGIN: Send promo link when player joins (true/false)\n");
                writer.write("# HYTALECHARTS_PROMO_ENABLED: Enable periodic promo link broadcasts (true/false)\n");
                writer.write("# HYTALECHARTS_PROMO_INTERVAL_MINUTES: How often to broadcast promo link (in minutes)\n");
                writer.write("# HYTALECHARTS_PROMO_PREFIX: Prefix before promo message (set to empty string to disable)\n");
                writer.write("# HYTALECHARTS_PROMO_MESSAGE: Promo link message text\n");
                writer.write("# HYTALECHARTS_PROMO_URL: URL for the clickable link\n");
                writer.write("HYTALECHARTS_SECRET=\n");
                writer.write("HYTALECHARTS_DEBUG=false\n");
                writer.write("HYTALECHARTS_PROMO_ON_LOGIN=false\n");
                writer.write("HYTALECHARTS_PROMO_ENABLED=false\n");
                writer.write("HYTALECHARTS_PROMO_INTERVAL_MINUTES=15\n");
                writer.write("HYTALECHARTS_PROMO_PREFIX=[hytalecharts.com] \n");
                writer.write("HYTALECHARTS_PROMO_MESSAGE=Vote for our server on HytaleCharts!\n");
                writer.write("HYTALECHARTS_PROMO_URL=https://hytalecharts.com\n");
                writer.write("\n");
                writer.write("# Optional: connect to a different Takaro instead of the public one.\n");
                writer.write("# The server keeps exactly one Takaro connection; this replaces the default URL.\n");
                writer.write("# TAKARO_WS_URL=wss://your-takaro-host/\n");
                writer.write("# Older setting, still read when TAKARO_WS_URL is not set: DEV_ENABLED=true with\n");
                writer.write("# DEV_WS_URL, DEV_IDENTITY_TOKEN and DEV_REGISTRATION_TOKEN.\n");
            }
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    /** The single Takaro endpoint and tokens to use; see {@link ConnectionTarget}. */
    public synchronized ConnectionTarget getConnectionTarget() {
        return ConnectionTarget.resolve(properties::getProperty);
    }

    public String getWsUrl() {
        return getConnectionTarget().url();
    }

    public String getIdentityToken() {
        return properties.getProperty("IDENTITY_TOKEN", "");
    }

    public String getRegistrationToken() {
        return properties.getProperty("REGISTRATION_TOKEN", "");
    }

    // Hidden from default config - will be used when Hytale API is available
    public String getHytaleApiUrl() {
        return properties.getProperty("HYTALE_API_URL", "https://api.hytale.com");
    }

    public String getHytaleApiToken() {
        return properties.getProperty("HYTALE_API_TOKEN", "");
    }


    /** How many game events to hold while the connection is down. */
    public int getEventQueueSize() {
        try {
            return Math.max(1, Integer.parseInt(properties.getProperty("EVENT_QUEUE_SIZE", "1000").trim()));
        } catch (NumberFormatException e) {
            return 1000;
        }
    }

    /**
     * How long to keep collecting a console command's output after its future completed, in
     * milliseconds. Hytale runs console commands synchronously on a ForkJoinPool thread,
     * so everything is already captured when the future resolves and the default is 0. Raise it
     * only for a command known to answer asynchronously; it is capped at 2000 ms so it can never
     * push a response past Takaro's 10 s request timeout.
     */
    public int getCommandSettleMs() {
        try {
            int value = Integer.parseInt(properties.getProperty("COMMAND_SETTLE_MS", "0").trim());
            return Math.min(2000, Math.max(0, value));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** Include Debug_/Test_/Dev_ entries in listItems and listEntities. */
    /** Serve the world map to Takaro (getMapInfo/getMapTile). On unless MAP_ENABLED=false. */
    public boolean isMapEnabled() {
        return !"false".equalsIgnoreCase(properties.getProperty("MAP_ENABLED", "true").trim());
    }

    public boolean isCatalogIncludeDebug() {
        return Boolean.parseBoolean(properties.getProperty("CATALOG_INCLUDE_DEBUG", "false").trim());
    }

    /** Trace every WebSocket frame at INFO. */
    public boolean isDebug() {
        return Boolean.parseBoolean(properties.getProperty("TAKARO_DEBUG", "false").trim());
    }

    /** Minimum level of a server log record that is forwarded to Takaro as a log event. */
    public String getLogForwardLevel() {
        return properties.getProperty("LOG_FORWARD_LEVEL", "INFO").trim();
    }

    /** Upper bound on forwarded log records per minute; 0 means unlimited. */
    public int getLogForwardMaxPerMin() {
        try {
            return Integer.parseInt(properties.getProperty("LOG_FORWARD_MAX_PER_MIN", "120").trim());
        } catch (NumberFormatException e) {
            return 120;
        }
    }

    public String getCommandPrefix() {
        return properties.getProperty("COMMAND_PREFIX", "!");
    }

    public String getCommandResponse() {
        return properties.getProperty("COMMAND_RESPONSE", "[cyan]Command[-] [green]{prefix}{command}[-]");
    }

    // HytaleCharts configuration
    public String getHytaleChartsSecret() {
        return properties.getProperty("HYTALECHARTS_SECRET", "");
    }

    public boolean getHytaleChartsDebug() {
        return Boolean.parseBoolean(properties.getProperty("HYTALECHARTS_DEBUG", "false"));
    }

    public boolean getHytaleChartsPromoOnLogin() {
        return Boolean.parseBoolean(properties.getProperty("HYTALECHARTS_PROMO_ON_LOGIN", "false"));
    }

    public boolean getHytaleChartsPromoEnabled() {
        return Boolean.parseBoolean(properties.getProperty("HYTALECHARTS_PROMO_ENABLED", "false"));
    }

    public int getHytaleChartsPromoIntervalMinutes() {
        return Integer.parseInt(properties.getProperty("HYTALECHARTS_PROMO_INTERVAL_MINUTES", "15"));
    }

    public String getHytaleChartsPromoPrefix() {
        return properties.getProperty("HYTALECHARTS_PROMO_PREFIX", "[hytalecharts.com] ");
    }

    public String getHytaleChartsPromoMessage() {
        return properties.getProperty("HYTALECHARTS_PROMO_MESSAGE", "Vote for our server on HytaleCharts!");
    }

    public String getHytaleChartsPromoUrl() {
        return properties.getProperty("HYTALECHARTS_PROMO_URL", "https://hytalecharts.com");
    }
}
