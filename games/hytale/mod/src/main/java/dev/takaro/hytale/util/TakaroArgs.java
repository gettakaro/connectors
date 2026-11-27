package dev.takaro.hytale.util;

import com.google.gson.Gson;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

/**
 * Null-safe accessor for the argument object of a Takaro request.
 *
 * <p>Takaro sends optional arguments in three different shapes depending on the caller:
 * <ul>
 *   <li>a module sends an explicit JSON {@code null} for an optional argument it did not set;</li>
 *   <li>the dashboard / API omits the key entirely;</li>
 *   <li>a value may arrive as a different JSON type than expected (a number as a string, say).</li>
 * </ul>
 * Reading such a value with Gson's {@code getAsInt()} / {@code getAsString()} /
 * {@code getAsJsonObject()} throws, and the connector's catch-all then reports a failure to
 * Takaro after the module has already told the player the action worked.
 *
 * <p>Every accessor here treats "missing", "explicit null" and "wrong type" identically: the
 * supplied default is returned. Player identity is resolved from both the nested
 * {@code {"player":{"gameId":...}}} form modules use and the flat {@code gameId} / {@code playerId}
 * form the dashboard uses.
 *
 * <p>No Hytale imports, so this is unit-testable without the server jar.
 */
public final class TakaroArgs {
    private static final Gson GSON = new Gson();

    private final JsonObject args;
    private final JsonObject payload;

    private TakaroArgs(JsonObject args, JsonObject payload) {
        this.args = args == null ? new JsonObject() : args;
        this.payload = payload == null ? new JsonObject() : payload;
    }

    /**
     * Parse the {@code args} member of a request payload. Takaro normally sends it as a JSON
     * <em>string</em>, but an object is accepted too. If there is no usable {@code args}, the
     * payload itself is used, so top-level {@code gameId} / {@code playerId} still resolve.
     */
    public static TakaroArgs of(JsonObject payload) {
        if (payload == null) {
            return new TakaroArgs(null, null);
        }
        JsonElement raw = payload.get("args");
        JsonObject parsed = null;
        if (raw != null && !raw.isJsonNull()) {
            try {
                if (raw.isJsonObject()) {
                    parsed = raw.getAsJsonObject();
                } else if (raw.isJsonPrimitive()) {
                    JsonElement fromString = GSON.fromJson(raw.getAsString(), JsonElement.class);
                    if (fromString != null && fromString.isJsonObject()) {
                        parsed = fromString.getAsJsonObject();
                    }
                }
            } catch (Exception ignored) {
                // fall through to payload-only mode
            }
        }
        return new TakaroArgs(parsed, payload);
    }

    /** The parsed argument object (never null). */
    public JsonObject raw() {
        return args;
    }

    /** True when the key is present in args with a value that is not JSON null. */
    public boolean has(String key) {
        return present(args, key) != null;
    }

    private static JsonElement present(JsonObject obj, String key) {
        if (obj == null || key == null) {
            return null;
        }
        JsonElement el = obj.get(key);
        return (el == null || el.isJsonNull()) ? null : el;
    }

    /** String value, or {@code def} if missing, explicitly null, or not a primitive. */
    public String str(String key, String def) {
        return str(args, key, def);
    }

    /** String value, or null if unusable. */
    public String str(String key) {
        return str(key, null);
    }

    /**
     * String value, treating an empty / whitespace-only string as absent. Used for
     * optional free text such as a kick or ban reason, where Takaro may send "" or null.
     */
    public String nonBlank(String key, String def) {
        String value = str(key, null);
        return (value == null || value.trim().isEmpty()) ? def : value;
    }

    private static String str(JsonObject obj, String key, String def) {
        JsonElement el = present(obj, key);
        if (el == null || !el.isJsonPrimitive()) {
            return def;
        }
        try {
            return el.getAsString();
        } catch (Exception e) {
            return def;
        }
    }

    /** Int value, or {@code def} if missing, explicitly null, or not parseable as a number. */
    public int intVal(String key, int def) {
        JsonElement el = present(args, key);
        if (el == null || !el.isJsonPrimitive()) {
            return def;
        }
        try {
            return el.getAsInt();
        } catch (Exception e) {
            try {
                return (int) Double.parseDouble(el.getAsString().trim());
            } catch (Exception e2) {
                return def;
            }
        }
    }

    /** Double value, or {@code def} if missing, explicitly null, or not parseable as a number. */
    public double dblVal(String key, double def) {
        JsonElement el = present(args, key);
        if (el == null || !el.isJsonPrimitive()) {
            return def;
        }
        try {
            return el.getAsDouble();
        } catch (Exception e) {
            try {
                return Double.parseDouble(el.getAsString().trim());
            } catch (Exception e2) {
                return def;
            }
        }
    }

    /** True if a numeric argument is present and usable (so a caller can reject a missing coordinate). */
    public boolean hasNumber(String key) {
        JsonElement el = present(args, key);
        if (el == null || !el.isJsonPrimitive()) {
            return false;
        }
        try {
            el.getAsDouble();
            return true;
        } catch (Exception e) {
            try {
                Double.parseDouble(el.getAsString().trim());
                return true;
            } catch (Exception e2) {
                return false;
            }
        }
    }

    /** Nested object, or null when missing / explicitly null / not an object. */
    public JsonObject obj(String key) {
        JsonElement el = present(args, key);
        return (el != null && el.isJsonObject()) ? el.getAsJsonObject() : null;
    }

    /** Walk a path of nested objects and return the string leaf, or null. */
    public String nested(String... path) {
        if (path == null || path.length == 0) {
            return null;
        }
        JsonObject cur = args;
        for (int i = 0; i < path.length - 1; i++) {
            JsonElement el = present(cur, path[i]);
            if (el == null || !el.isJsonObject()) {
                return null;
            }
            cur = el.getAsJsonObject();
        }
        return str(cur, path[path.length - 1], null);
    }

    /**
     * The target player's game id, from any of the shapes Takaro uses:
     * nested {@code player.gameId}, flat {@code gameId} / {@code playerId} in args, or the same
     * keys at the top level of the request payload.
     */
    public String playerGameId() {
        String id = nested("player", "gameId");
        if (id == null) {
            id = str("gameId", null);
        }
        if (id == null) {
            id = str("playerId", null);
        }
        if (id == null) {
            id = str(payload, "gameId", null);
        }
        if (id == null) {
            id = str(payload, "playerId", null);
        }
        return (id != null && !id.trim().isEmpty()) ? id.trim() : null;
    }

    /** A second player's game id, for teleportPlayerToPlayer. */
    public String targetGameId() {
        String id = nested("target", "gameId");
        if (id == null) {
            id = str("targetGameId", null);
        }
        if (id == null) {
            id = str("targetPlayerId", null);
        }
        return (id != null && !id.trim().isEmpty()) ? id.trim() : null;
    }

    /** The player name, from nested {@code player.name} or flat {@code name}. */
    public String playerName() {
        String name = nested("player", "name");
        if (name == null) {
            name = str("name", null);
        }
        return (name != null && !name.trim().isEmpty()) ? name.trim() : null;
    }

    /** The private-message recipient's game id ({@code opts.recipient.gameId}). */
    public String recipientGameId() {
        return nested("opts", "recipient", "gameId");
    }
}
