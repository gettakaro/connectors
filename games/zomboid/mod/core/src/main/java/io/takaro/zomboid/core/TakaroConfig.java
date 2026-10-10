package io.takaro.zomboid.core;

import java.util.Map;
import java.util.Objects;

/**
 * The connector settings in use. One instance is shared by the connector and its socket;
 * a config reload copies the new values into it ({@link #copyFrom}), so the fields are
 * volatile.
 */
public class TakaroConfig {
    public static final String DEFAULT_WS_URL = "wss://connect.takaro.io/";

    private volatile String wsUrl;
    private volatile String identityToken;
    private volatile String registrationToken;
    private volatile String serverChatName;
    private volatile String configFileHint;
    private volatile boolean reconnectEnabled = true;
    private volatile long reconnectDelay = 5000;
    private volatile long maxReconnectDelay = 300000;
    private volatile double backoffMultiplier = 1.5;
    private volatile boolean debugEnabled = false;

    public String getWsUrl() { return wsUrl; }
    public void setWsUrl(String wsUrl) { this.wsUrl = wsUrl; }

    public String getIdentityToken() { return identityToken; }
    public void setIdentityToken(String identityToken) { this.identityToken = identityToken; }

    public String getRegistrationToken() { return registrationToken; }
    public void setRegistrationToken(String registrationToken) { this.registrationToken = registrationToken; }

    /** Chat sender name for connector-sent messages (falls back to the game server name, then "Server"). */
    public String getServerChatName() { return serverChatName; }
    public void setServerChatName(String serverChatName) { this.serverChatName = serverChatName; }

    /** The config file a server owner edits, named in the log when a token is missing or rejected. */
    public String getConfigFileHint() { return configFileHint != null ? configFileHint : "TakaroConfig.txt"; }
    public void setConfigFileHint(String configFileHint) { this.configFileHint = configFileHint; }

    public boolean isReconnectEnabled() { return reconnectEnabled; }
    public void setReconnectEnabled(boolean reconnectEnabled) { this.reconnectEnabled = reconnectEnabled; }

    public long getReconnectDelay() { return reconnectDelay; }
    public void setReconnectDelay(long reconnectDelay) { this.reconnectDelay = reconnectDelay; }

    public long getMaxReconnectDelay() { return maxReconnectDelay; }
    public void setMaxReconnectDelay(long maxReconnectDelay) { this.maxReconnectDelay = maxReconnectDelay; }

    public double getBackoffMultiplier() { return backoffMultiplier; }
    public void setBackoffMultiplier(double backoffMultiplier) { this.backoffMultiplier = backoffMultiplier; }

    public boolean isDebugEnabled() { return debugEnabled; }
    public void setDebugEnabled(boolean debugEnabled) { this.debugEnabled = debugEnabled; }

    public static boolean isSet(String value) {
        return value != null && !value.isEmpty();
    }

    /** True when both would open the same Takaro connection (URL and both tokens). */
    public boolean sameConnection(TakaroConfig other) {
        return other != null
                && Objects.equals(wsUrl, other.wsUrl)
                && Objects.equals(identityToken, other.identityToken)
                && Objects.equals(registrationToken, other.registrationToken);
    }

    /** Takes over the settings a config file can change; the reconnect tuning stays. */
    public void copyFrom(TakaroConfig other) {
        this.wsUrl = other.wsUrl;
        this.identityToken = other.identityToken;
        this.registrationToken = other.registrationToken;
        this.serverChatName = other.serverChatName;
        this.configFileHint = other.configFileHint;
        this.debugEnabled = other.debugEnabled;
    }

    public void applyEnvOverrides() {
        applyEnvOverrides(System.getenv());
    }

    /** The TAKARO_* environment wins over whatever the config file set. */
    public void applyEnvOverrides(Map<String, String> env) {
        String wsUrlEnv = env.get("TAKARO_WS_URL");
        if (isSet(wsUrlEnv)) {
            this.wsUrl = wsUrlEnv;
        }
        String identityEnv = env.get("TAKARO_IDENTITY_TOKEN");
        if (isSet(identityEnv)) {
            this.identityToken = identityEnv;
        }
        String registrationEnv = env.get("TAKARO_REGISTRATION_TOKEN");
        if (isSet(registrationEnv)) {
            this.registrationToken = registrationEnv;
        }
        String chatNameEnv = env.get("TAKARO_SERVER_CHAT_NAME");
        if (isSet(chatNameEnv)) {
            serverChatName = chatNameEnv;
        }
        String debugEnv = env.get("TAKARO_DEBUG");
        if (isSet(debugEnv)) {
            this.debugEnabled = "true".equalsIgnoreCase(debugEnv) || "1".equals(debugEnv);
        }
    }
}
