package io.takaro.zomboid.agent;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.*;

class ConfigFilesTest {

    @Test
    void withIdentityRewritesOnlyThatLine() {
        String text = "# keep me\nwsUrl=wss://x/\nidentityToken=   # fill in\nregistrationToken=abc\n";
        String out = ConfigFiles.withIdentity(text, "id-1");
        assertEquals("# keep me\nwsUrl=wss://x/\nidentityToken=id-1 # fill in\nregistrationToken=abc\n", out);
        assertEquals("id-1", ConfigFiles.parse(out).get("identityToken"));
    }

    @Test
    void withIdentityAppendsWhenTheKeyIsMissing() {
        assertEquals("registrationToken=abc\nidentityToken=id-1\n",
                ConfigFiles.withIdentity("registrationToken=abc", "id-1"));
    }

    @Test
    void shippedTemplateHasEmptyTokensAndTheDefaultUrl() throws Exception {
        Map<String, String> values = ConfigFiles.parse(ConfigFiles.template());
        assertEquals("", values.get("registrationToken"));
        assertEquals("", values.get("identityToken"));
        assertEquals("wss://connect.takaro.io/", values.get("wsUrl"));
        assertEquals("false", values.get("debug"));
    }

    @Test
    void savedCopyHoldsOnlyTheTokens() {
        ConfigFiles.Resolved r = ConfigFiles.resolve(Map.of("registrationToken", "reg", "identityToken", "id",
                "debug", "true"), Map.of(), Map.of(), null);
        Map<String, String> saved = ConfigFiles.parse(ConfigFiles.renderSaved(r));
        assertEquals(Map.of("registrationToken", "reg", "identityToken", "id"), saved);
    }

    @Test
    void nothingToSaveWhenNoTokenCameFromAFile() {
        ConfigFiles.Resolved r = ConfigFiles.resolve(Map.of(), Map.of(),
                Map.of("TAKARO_REGISTRATION_TOKEN", "env", "TAKARO_IDENTITY_TOKEN", "env"), null);
        assertNull(ConfigFiles.renderSaved(r));
    }

    @Test
    void rewritingAFileKeepsItsModeAndTheSavedCopyIsOwnerOnly(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, "identityToken=\n");
        Files.setPosixFilePermissions(user, PosixFilePermissions.fromString("rw-rw-r--"));
        ConfigFiles.writeAtomically(user, "identityToken=x\n", false);
        assertEquals("rw-rw-r--", PosixFilePermissions.toString(Files.getPosixFilePermissions(user)));

        Path fresh = dir.resolve("new.txt");
        ConfigFiles.writeAtomically(fresh, "a=b\n", false);
        assertEquals("rw-r--r--", PosixFilePermissions.toString(Files.getPosixFilePermissions(fresh)));
        assertEquals(Files.getOwner(dir), Files.getOwner(fresh));

        Path saved = dir.resolve("TakaroConfig.saved.txt");
        ConfigFiles.writeAtomically(saved, "registrationToken=r\n", true);
        assertEquals("rw-------", PosixFilePermissions.toString(Files.getPosixFilePermissions(saved)));
    }
}
