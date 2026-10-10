package io.takaro.minecraft.core.config;

import io.takaro.minecraft.core.TakaroConfig;

import java.io.IOException;
import java.io.StringReader;
import java.util.Properties;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** NeoForge: {@code config/takaro.properties}. */
public final class PropertiesConfigFormat implements ConfigFormat {

    private static final String IDENTITY_KEY = "takaro.authentication.identity_token";
    private static final String REGISTRATION_KEY = "takaro.authentication.registration_token";
    private static final Pattern EMPTY_IDENTITY =
            Pattern.compile("(?m)^[ \\t]*takaro\\.authentication\\.identity_token[ \\t]*[=:]?[ \\t]*$");

    @Override
    public TakaroConfig parse(String text) throws IOException {
        Properties props = new Properties();
        props.load(new StringReader(text));
        if (!props.containsKey(REGISTRATION_KEY)) {
            throw new IllegalArgumentException(REGISTRATION_KEY + " is missing");
        }
        TakaroConfig config = new TakaroConfig();
        config.setWsUrl(props.getProperty("takaro.websocket.url", ""));
        config.setIdentityToken(props.getProperty(IDENTITY_KEY, ""));
        config.setRegistrationToken(props.getProperty(REGISTRATION_KEY, ""));
        config.setReconnectEnabled(Boolean.parseBoolean(props.getProperty("takaro.reconnect.enabled", "true").trim()));
        config.setReconnectDelay(Long.parseLong(props.getProperty("takaro.reconnect.delay", "5000").trim()));
        config.setMaxReconnectDelay(Long.parseLong(props.getProperty("takaro.reconnect.max_delay", "300000").trim()));
        config.setBackoffMultiplier(Double.parseDouble(props.getProperty("takaro.reconnect.backoff_multiplier", "1.5").trim()));
        config.setDebugEnabled(Boolean.parseBoolean(props.getProperty("takaro.debug", "false").trim()));
        return config;
    }

    @Override
    public String defaultText() {
        return "# Takaro connector settings. Saving this file is enough; the connector picks up\n"
                + "# changes within a few seconds, no restart needed.\n"
                + "takaro.websocket.url=" + TakaroConfig.DEFAULT_WS_URL + "\n"
                + "# Filled in by the connector on first start. Keep it; it is how Takaro knows this server.\n"
                + IDENTITY_KEY + "=\n"
                + "# Paste the registration token from Takaro here.\n"
                + REGISTRATION_KEY + "=\n"
                + "takaro.reconnect.enabled=true\n"
                + "takaro.reconnect.delay=5000\n"
                + "takaro.reconnect.max_delay=300000\n"
                + "takaro.reconnect.backoff_multiplier=1.5\n"
                + "takaro.debug=false\n";
    }

    @Override
    public String withIdentity(String text, String identity) {
        String line = IDENTITY_KEY + "=" + identity;
        Matcher m = EMPTY_IDENTITY.matcher(text);
        if (m.find()) {
            return text.substring(0, m.start()) + line + text.substring(m.end());
        }
        return text + (text.isEmpty() || text.endsWith("\n") ? "" : "\n") + line + "\n";
    }
}
