package io.takaro.zomboid.agent;

/**
 * Scopes non-Steam (-nosteam) account identities to this server; set once in premain.
 *
 * <p>Kept apart from {@link Pz} on purpose: premain runs in JVMs without the game jar on the
 * classpath, and touching Pz there fails with NoClassDefFoundError on the game types it links.
 */
final class LocalAccounts {
    private static volatile String scope = "";

    private LocalAccounts() {}

    static void setScope(String value) {
        scope = value != null ? value : "";
    }

    static String scope() {
        return scope;
    }
}
