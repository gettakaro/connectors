package dev.takaro.hytale.config;

import java.util.function.Function;

/**
 * The one Takaro endpoint this server connects to, and the tokens it identifies with.
 *
 * <p>Order: {@code TAKARO_WS_URL} with {@code IDENTITY_TOKEN}/{@code REGISTRATION_TOKEN}; else
 * {@code DEV_ENABLED=true} with {@code DEV_WS_URL} and the {@code DEV_*} tokens; else the public
 * Takaro URL with {@code IDENTITY_TOKEN}/{@code REGISTRATION_TOKEN}. The plugin never opens a
 * second connection, so a server pointed at another Takaro never also reaches the public one.
 */
public final class ConnectionTarget {
    public static final String DEFAULT_URL = "wss://connect.takaro.io/";
    public static final String DEFAULT_DEV_URL = "wss://connect.next.takaro.dev/";

    private final String url;
    private final String identityToken;
    private final String registrationToken;
    private final boolean dev;

    private ConnectionTarget(String url, String identityToken, String registrationToken, boolean dev) {
        this.url = url;
        this.identityToken = identityToken;
        this.registrationToken = registrationToken;
        this.dev = dev;
    }

    /** @param property reads one config value; returns null when it is not set */
    public static ConnectionTarget resolve(Function<String, String> property) {
        String url = trimmed(property.apply("TAKARO_WS_URL"));
        if (!url.isEmpty()) {
            return new ConnectionTarget(url, trimmed(property.apply("IDENTITY_TOKEN")),
                trimmed(property.apply("REGISTRATION_TOKEN")), false);
        }
        if (Boolean.parseBoolean(trimmed(property.apply("DEV_ENABLED")))) {
            String devUrl = trimmed(property.apply("DEV_WS_URL"));
            return new ConnectionTarget(devUrl.isEmpty() ? DEFAULT_DEV_URL : devUrl,
                trimmed(property.apply("DEV_IDENTITY_TOKEN")),
                trimmed(property.apply("DEV_REGISTRATION_TOKEN")), true);
        }
        return new ConnectionTarget(DEFAULT_URL, trimmed(property.apply("IDENTITY_TOKEN")),
            trimmed(property.apply("REGISTRATION_TOKEN")), false);
    }

    private static String trimmed(String value) {
        return value == null ? "" : value.trim();
    }

    public String url() {
        return url;
    }

    public String identityToken() {
        return identityToken;
    }

    public String registrationToken() {
        return registrationToken;
    }

    /** True when the DEV_* settings chose this target. */
    public boolean dev() {
        return dev;
    }
}
