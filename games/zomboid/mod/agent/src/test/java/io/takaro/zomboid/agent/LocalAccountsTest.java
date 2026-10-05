package io.takaro.zomboid.agent;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

/** The agent test classpath has no game jar, like the launcher JVMs premain also runs in. */
class LocalAccountsTest {

    @Test
    void scopeIsSettableWithoutTheGameJar() {
        LocalAccounts.setScope("takaro-dev-zomboid");
        assertEquals("takaro-dev-zomboid", LocalAccounts.scope());
        LocalAccounts.setScope(null);
        assertEquals("", LocalAccounts.scope());
    }
}
