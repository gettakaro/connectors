package io.takaro.minecraft.core;

public class TakaroConfig {
    private String wsUrl;
    private String identityToken;
    private String registrationToken;
    private boolean reconnectEnabled = true;
    private long reconnectDelay = 5000;
    private long maxReconnectDelay = 300000;
    private double backoffMultiplier = 1.5;
    private boolean debugEnabled = false;

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

    public void applyEnvOverrides() {
        String wsUrlEnv = System.getenv("TAKARO_WS_URL");
        if (wsUrlEnv != null && !wsUrlEnv.isEmpty()) {
            this.wsUrl = wsUrlEnv;
        }
        String identityEnv = System.getenv("TAKARO_IDENTITY_TOKEN");
        if (identityEnv != null && !identityEnv.isEmpty()) {
            this.identityToken = identityEnv;
        }
        String registrationEnv = System.getenv("TAKARO_REGISTRATION_TOKEN");
        if (registrationEnv != null && !registrationEnv.isEmpty()) {
            this.registrationToken = registrationEnv;
        }
        String debugEnv = System.getenv("TAKARO_DEBUG");
        if (debugEnv != null && !debugEnv.isEmpty()) {
            this.debugEnabled = "true".equalsIgnoreCase(debugEnv) || "1".equals(debugEnv);
        }
    }
}
