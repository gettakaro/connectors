package io.takaro.minecraft.core;

import java.util.Objects;
import java.util.function.Function;

/**
 * The settings in use. Fields are volatile because the config watcher replaces them while the
 * websocket threads read them.
 */
public class TakaroConfig {
    /** Used when the config file leaves the URL empty, as files written before 0.4 do. */
    public static final String DEFAULT_WS_URL = "wss://connect.takaro.io/";

    private volatile String wsUrl;
    private volatile String identityToken;
    private volatile String registrationToken;
    private volatile boolean reconnectEnabled = true;
    private volatile long reconnectDelay = 5000;
    private volatile long maxReconnectDelay = 300000;
    private volatile double backoffMultiplier = 1.5;
    private volatile boolean debugEnabled = false;
    private volatile String targetPolicy = "enforce";

    public String getWsUrl() { return wsUrl; }
    public void setWsUrl(String wsUrl) { this.wsUrl = wsUrl; }

    public String getIdentityToken() { return identityToken; }
    public void setIdentityToken(String identityToken) { this.identityToken = identityToken; }

    public String getRegistrationToken() { return registrationToken; }
    public void setRegistrationToken(String registrationToken) { this.registrationToken = registrationToken; }

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

    /** enforce (default), warn or off — how strictly the build target must match this server. */
    public String getTargetPolicy() { return targetPolicy; }
    public void setTargetPolicy(String targetPolicy) { this.targetPolicy = targetPolicy; }

    public void applyEnvOverrides() {
        applyEnvOverrides(System::getenv);
    }

    /** A TAKARO_* variable that is set and not empty wins over the file. */
    public void applyEnvOverrides(Function<String, String> env) {
        String wsUrlEnv = env.apply("TAKARO_WS_URL");
        if (wsUrlEnv != null && !wsUrlEnv.isEmpty()) {
            this.wsUrl = wsUrlEnv;
        }
        String identityEnv = env.apply("TAKARO_IDENTITY_TOKEN");
        if (identityEnv != null && !identityEnv.isEmpty()) {
            this.identityToken = identityEnv;
        }
        String registrationEnv = env.apply("TAKARO_REGISTRATION_TOKEN");
        if (registrationEnv != null && !registrationEnv.isEmpty()) {
            this.registrationToken = registrationEnv;
        }
        String policyEnv = env.apply("TAKARO_TARGET_POLICY");
        if (policyEnv != null && !policyEnv.isEmpty()) {
            this.targetPolicy = policyEnv;
        }
        String debugEnv = env.apply("TAKARO_DEBUG");
        if (debugEnv != null && !debugEnv.isEmpty()) {
            this.debugEnabled = "true".equalsIgnoreCase(debugEnv) || "1".equals(debugEnv);
        }
    }

    /** True when switching from this config to {@code other} needs a new connection. */
    public boolean sameConnection(TakaroConfig other) {
        return Objects.equals(wsUrl, other.wsUrl)
                && Objects.equals(identityToken, other.identityToken)
                && Objects.equals(registrationToken, other.registrationToken)
                && Objects.equals(targetPolicy, other.targetPolicy)
                && reconnectEnabled == other.reconnectEnabled;
    }

    public boolean sameSettings(TakaroConfig other) {
        return sameConnection(other)
                && reconnectDelay == other.reconnectDelay
                && maxReconnectDelay == other.maxReconnectDelay
                && backoffMultiplier == other.backoffMultiplier
                && debugEnabled == other.debugEnabled;
    }

    public void copyFrom(TakaroConfig other) {
        wsUrl = other.wsUrl;
        identityToken = other.identityToken;
        registrationToken = other.registrationToken;
        reconnectEnabled = other.reconnectEnabled;
        reconnectDelay = other.reconnectDelay;
        maxReconnectDelay = other.maxReconnectDelay;
        backoffMultiplier = other.backoffMultiplier;
        debugEnabled = other.debugEnabled;
        targetPolicy = other.targetPolicy;
    }
}
