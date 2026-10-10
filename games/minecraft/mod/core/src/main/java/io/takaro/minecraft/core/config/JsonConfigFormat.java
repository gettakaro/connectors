package io.takaro.minecraft.core.config;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.takaro.minecraft.core.TakaroConfig;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Fabric: {@code config/takaro.json}. */
public final class JsonConfigFormat implements ConfigFormat {

    private static final Pattern EMPTY_IDENTITY =
            Pattern.compile("(\"identity_token\"[ \\t]*:[ \\t]*)(\"\"|null)");

    @Override
    public TakaroConfig parse(String text) {
        JsonElement root = JsonParser.parseString(text);
        if (!root.isJsonObject()) {
            throw new IllegalArgumentException("not a JSON object");
        }
        JsonObject json = root.getAsJsonObject();
        JsonObject ws = object(json, "websocket");
        JsonObject auth = object(json, "authentication");
        JsonObject reconnect = object(json, "reconnect");
        JsonObject settings = object(json, "settings");
        if (!auth.has("registration_token")) {
            throw new IllegalArgumentException("authentication.registration_token is missing");
        }

        TakaroConfig config = new TakaroConfig();
        config.setWsUrl(string(ws, "url"));
        config.setIdentityToken(string(auth, "identity_token"));
        config.setRegistrationToken(string(auth, "registration_token"));
        if (present(reconnect, "enabled")) config.setReconnectEnabled(reconnect.get("enabled").getAsBoolean());
        if (present(reconnect, "delay")) config.setReconnectDelay(reconnect.get("delay").getAsLong());
        if (present(reconnect, "max_delay")) config.setMaxReconnectDelay(reconnect.get("max_delay").getAsLong());
        if (present(reconnect, "backoff_multiplier")) {
            config.setBackoffMultiplier(reconnect.get("backoff_multiplier").getAsDouble());
        }
        config.setDebugEnabled(present(settings, "debug") && settings.get("debug").getAsBoolean());
        return config;
    }

    @Override
    public String defaultText() {
        return "{\n"
                + "  \"websocket\": {\n"
                + "    \"url\": \"" + TakaroConfig.DEFAULT_WS_URL + "\"\n"
                + "  },\n"
                + "  \"authentication\": {\n"
                + "    \"identity_token\": \"\",\n"
                + "    \"registration_token\": \"\"\n"
                + "  },\n"
                + "  \"reconnect\": {\n"
                + "    \"enabled\": true,\n"
                + "    \"delay\": 5000,\n"
                + "    \"max_delay\": 300000,\n"
                + "    \"backoff_multiplier\": 1.5\n"
                + "  },\n"
                + "  \"settings\": {\n"
                + "    \"debug\": false\n"
                + "  }\n"
                + "}\n";
    }

    @Override
    public String withIdentity(String text, String identity) {
        Matcher m = EMPTY_IDENTITY.matcher(text);
        if (m.find()) {
            return text.substring(0, m.start()) + m.group(1) + "\"" + identity + "\"" + text.substring(m.end());
        }
        JsonObject json = JsonParser.parseString(text).getAsJsonObject();
        JsonObject auth = object(json, "authentication");
        auth.addProperty("identity_token", identity);
        json.add("authentication", auth);
        return new GsonBuilder().setPrettyPrinting().create().toJson(json) + "\n";
    }

    private static JsonObject object(JsonObject json, String key) {
        return json.has(key) && json.get(key).isJsonObject() ? json.getAsJsonObject(key) : new JsonObject();
    }

    private static boolean present(JsonObject json, String key) {
        return json.has(key) && !json.get(key).isJsonNull();
    }

    private static String string(JsonObject json, String key) {
        return present(json, key) ? json.get(key).getAsString() : "";
    }
}
