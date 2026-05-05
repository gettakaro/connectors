package dev.takaro.hytale.api;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import okhttp3.*;
import dev.takaro.hytale.TakaroPlugin;

import java.io.IOException;
import java.util.concurrent.TimeUnit;

/**
 * Client for Hytale's official first-party API endpoints
 *
 * Authenticated servers have access to:
 * - UUID ↔ Name Lookup
 * - Game Version
 * - Player Profile (cosmetics, avatar, public profile)
 * - Server Telemetry
 * - Report (ToS violations)
 * - Payments
 *
 * Under consideration:
 * - Global Sanctions
 * - Friends List
 * - Webhook Subscriptions
 */
public class HytaleApiClient {
    private final TakaroPlugin plugin;
    private final OkHttpClient httpClient;
    private final Gson gson;
    private final String apiBaseUrl;
    private String authToken; // Server authentication token

    public HytaleApiClient(TakaroPlugin plugin, String apiBaseUrl) {
        this.plugin = plugin;
        this.apiBaseUrl = apiBaseUrl;
        this.gson = new Gson();

        this.httpClient = new OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            .writeTimeout(30, TimeUnit.SECONDS)
            .build();
    }

    /**
     * Set the authentication token for API requests
     */
    public void setAuthToken(String token) {
        this.authToken = token;
    }

    /**
     * UUID ↔ Name Lookup
     * Resolve player names to UUIDs and vice versa
     * Supports single and bulk lookups
     */
    public JsonObject lookupPlayerByName(String playerName) throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/player/lookup/name/" + playerName)
            .addHeader("Authorization", "Bearer " + authToken)
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player lookup failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * UUID ↔ Name Lookup (by UUID)
     */
    public JsonObject lookupPlayerByUUID(String uuid) throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/player/lookup/uuid/" + uuid)
            .addHeader("Authorization", "Bearer " + authToken)
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player lookup failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Bulk player lookup
     */
    public JsonObject bulkLookupPlayers(String[] playerNames) throws IOException {
        JsonObject body = new JsonObject();
        body.add("names", gson.toJsonTree(playerNames));

        Request request = new Request.Builder()
            .url(apiBaseUrl + "/player/lookup/bulk")
            .addHeader("Authorization", "Bearer " + authToken)
            .post(RequestBody.create(body.toString(), MediaType.parse("application/json")))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Bulk lookup failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Game Version
     * Query current game version, protocol version, and check for updates
     */
    public JsonObject getGameVersion() throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/version")
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Version check failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Player Profile
     * Fetch player profile data including cosmetics, avatar renders, and public profile information
     */
    public JsonObject getPlayerProfile(String uuid) throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/player/profile/" + uuid)
            .addHeader("Authorization", "Bearer " + authToken)
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Profile fetch failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Server Telemetry
     * Report server status, player count, and metadata for discovery integration
     */
    public boolean reportTelemetry(int playerCount, String serverStatus, JsonObject metadata) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("playerCount", playerCount);
        body.addProperty("status", serverStatus);
        if (metadata != null) {
            body.add("metadata", metadata);
        }

        Request request = new Request.Builder()
            .url(apiBaseUrl + "/server/telemetry")
            .addHeader("Authorization", "Bearer " + authToken)
            .post(RequestBody.create(body.toString(), MediaType.parse("application/json")))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Telemetry report failed: " + response.code());
                return false;
            }
            return true;
        }
    }

    /**
     * Report
     * Report players for ToS violations
     */
    public boolean reportPlayer(String reportedUuid, String reason, String evidence) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("reportedUuid", reportedUuid);
        body.addProperty("reason", reason);
        body.addProperty("evidence", evidence);

        Request request = new Request.Builder()
            .url(apiBaseUrl + "/report")
            .addHeader("Authorization", "Bearer " + authToken)
            .post(RequestBody.create(body.toString(), MediaType.parse("application/json")))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Player report failed: " + response.code());
                return false;
            }
            return true;
        }
    }

    /**
     * Payments
     * Process payments using built-in payment gate
     */
    public JsonObject processPayment(String playerUuid, double amount, String currency, String itemDescription) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("playerUuid", playerUuid);
        body.addProperty("amount", amount);
        body.addProperty("currency", currency);
        body.addProperty("description", itemDescription);

        Request request = new Request.Builder()
            .url(apiBaseUrl + "/payments/process")
            .addHeader("Authorization", "Bearer " + authToken)
            .post(RequestBody.create(body.toString(), MediaType.parse("application/json")))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Payment processing failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Global Sanctions (Under Consideration)
     * Query whether a player has platform-level sanctions
     */
    public JsonObject checkGlobalSanctions(String uuid) throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/sanctions/" + uuid)
            .addHeader("Authorization", "Bearer " + authToken)
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                if (response.code() == 404) {
                    // Endpoint not yet available
                    plugin.getLogger().at(java.util.logging.Level.INFO).log("Global sanctions endpoint not yet available");
                    return null;
                }
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Sanctions check failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Friends List (Under Consideration)
     * Retrieve a player's friends list (with appropriate permissions)
     */
    public JsonObject getFriendsList(String uuid) throws IOException {
        Request request = new Request.Builder()
            .url(apiBaseUrl + "/player/friends/" + uuid)
            .addHeader("Authorization", "Bearer " + authToken)
            .get()
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                if (response.code() == 404) {
                    // Endpoint not yet available
                    plugin.getLogger().at(java.util.logging.Level.INFO).log("Friends list endpoint not yet available");
                    return null;
                }
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Friends list fetch failed: " + response.code());
                return null;
            }
            return gson.fromJson(response.body().string(), JsonObject.class);
        }
    }

    /**
     * Webhook Subscriptions (Under Consideration)
     * Subscribe to push notifications for events
     */
    public boolean subscribeWebhook(String webhookUrl, String[] eventTypes) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("webhookUrl", webhookUrl);
        body.add("events", gson.toJsonTree(eventTypes));

        Request request = new Request.Builder()
            .url(apiBaseUrl + "/webhooks/subscribe")
            .addHeader("Authorization", "Bearer " + authToken)
            .post(RequestBody.create(body.toString(), MediaType.parse("application/json")))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                if (response.code() == 404) {
                    // Endpoint not yet available
                    plugin.getLogger().at(java.util.logging.Level.INFO).log("Webhook subscription endpoint not yet available");
                    return false;
                }
                plugin.getLogger().at(java.util.logging.Level.WARNING).log("Webhook subscription failed: " + response.code());
                return false;
            }
            return true;
        }
    }

    public void shutdown() {
        // OkHttp client will clean up automatically
    }
}
