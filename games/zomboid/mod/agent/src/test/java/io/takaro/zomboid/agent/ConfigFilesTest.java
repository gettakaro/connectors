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
    void bundledTemplateHasEmptyTokensAndTheDefaultUrl() throws Exception {
        Map<String, String> values = ConfigFiles.parse(ConfigFiles.template());
        assertEquals("", values.get("registrationToken"));
        assertEquals("", values.get("identityToken"));
        assertEquals("wss://connect.takaro.io/", values.get("wsUrl"));
        assertEquals("false", values.get("debug"));
        assertEquals("1", values.get("protocol"));
    }

    @Test
    void rewritingAFileKeepsItsModeAndANewOneTakesTheFolderOwner(@TempDir Path dir) throws Exception {
        Path user = dir.resolve("TakaroConfig.txt");
        Files.writeString(user, "identityToken=\n");
        Files.setPosixFilePermissions(user, PosixFilePermissions.fromString("rw-rw-r--"));
        ConfigFiles.writeAtomically(user, "identityToken=x\n");
        assertEquals("rw-rw-r--", PosixFilePermissions.toString(Files.getPosixFilePermissions(user)));

        Path fresh = dir.resolve("new.txt");
        ConfigFiles.writeAtomically(fresh, "a=b\n");
        assertEquals("rw-r--r--", PosixFilePermissions.toString(Files.getPosixFilePermissions(fresh)));
        assertEquals(Files.getOwner(dir), Files.getOwner(fresh));
    }
}
