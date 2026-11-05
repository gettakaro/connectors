// Runs the connector's own config region (lifted out of mod/TakaroConnector.cs by run.sh)
// against the cases a server owner actually goes through.
using System;
using System.Collections.Generic;

internal static class Program
{
    private static int _checks;
    private static int _failures;

    private static void Check(bool ok, string what)
    {
        _checks++;
        if (ok) return;
        _failures++;
        Console.WriteLine("  FAIL: " + what);
    }

    private static Func<string, string> Env(params string[] pairs)
    {
        var map = new Dictionary<string, string>();
        for (var i = 0; i + 1 < pairs.Length; i += 2) map[pairs[i]] = pairs[i + 1];
        return name => map.TryGetValue(name, out var value) ? value : null;
    }

    private static readonly Func<string, string> NoEnv = Env();

    private static string File(string registration, string identity)
    {
        return "{\n  \"RegistrationToken\": \"" + registration + "\",\n  \"IdentityToken\": \"" + identity +
            "\",\n  \"WebSocketUrl\": \"wss://connect.takaro.io/\",\n  \"Debug\": false\n}\n";
    }

    private static int Main()
    {
        // Fresh install: no file, an empty one, or the "{}" a framework may leave.
        foreach (var text in new[] { null, "", "  \n", "{}" })
            Check(text == null || Config.IsFreshConfig(text), "fresh: '" + text + "' counts as a file the plugin never wrote");
        Check(!Config.IsFreshConfig("{ \"WebSocketUrl\": \"ws://10.0.0.5/\" }"), "fresh: a file with only a custom URL is kept");
        Check(!Config.IsFreshConfig("{ \"Debug\": true }"), "fresh: a file with only Debug is kept");
        var fresh = Config.FirstConfig(() => "11111111-2222-3333-4444-555555555555");
        Check(fresh.RegistrationToken == "", "fresh: registration token is empty");
        Check(fresh.IdentityToken == "11111111-2222-3333-4444-555555555555", "fresh: a new identity is generated");
        var freshText = Config.RenderConfigText(fresh);
        Check(!Config.IsFreshConfig(freshText), "fresh: the written file is not fresh on the next load");
        var reread = Config.ParseConfigText(freshText);
        Check(reread.IdentityToken == fresh.IdentityToken && reread.RegistrationToken == "" &&
              reread.WsUrl == Config.DefaultWsUrl && reread.Debug == false, "fresh: the written file reads back as written");
        var freshSettings = Config.ResolveSettings(reread, NoEnv, null);
        Check(freshSettings.RegistrationToken == "" && !freshSettings.RegistrationFromEnv, "fresh: no token means no connection");
        Check(freshSettings.IdentityToken == fresh.IdentityToken, "fresh: the generated identity is used");

        // Env still wins field by field; the file gets its own identity all the same.
        var envFresh = Config.FirstConfig(() => "gen-1");
        Check(envFresh.IdentityToken == "gen-1", "env: a new file gets a generated identity even with env tokens set");
        var envBothSettings = Config.ResolveSettings(envFresh,
            Env(Config.EnvRegistrationToken, "reg", Config.EnvIdentityToken, "my-rust-1", Config.EnvWsUrl, "ws://x/", Config.EnvDebug, "TRUE"), null);
        Check(envBothSettings.IdentityToken == "my-rust-1" && envBothSettings.IdentityFromEnv, "env: TAKARO_IDENTITY_TOKEN wins");
        Check(envBothSettings.RegistrationToken == "reg" && envBothSettings.RegistrationFromEnv, "env: TAKARO_REGISTRATION_TOKEN wins");
        Check(envBothSettings.WsUrl == "ws://x/", "env: TAKARO_WS_URL wins");
        Check(envBothSettings.Debug, "env: TAKARO_DEBUG=TRUE is true");

        // Env wins over the file field by field; an empty env var does not count as set.
        var file = Config.ParseConfigText(File("file-reg", "file-id"));
        var envWins = Config.ResolveSettings(file, Env(Config.EnvRegistrationToken, "env-reg"), null);
        Check(envWins.RegistrationToken == "env-reg" && envWins.RegistrationFromEnv, "env: TAKARO_REGISTRATION_TOKEN wins over the file");
        Check(envWins.IdentityToken == "file-id" && !envWins.IdentityFromEnv, "env: the file identity is used when the env has none");
        var emptyEnv = Config.ResolveSettings(file, Env(Config.EnvRegistrationToken, "", Config.EnvIdentityToken, ""), null);
        Check(emptyEnv.RegistrationToken == "file-reg" && emptyEnv.IdentityToken == "file-id", "env: an empty env var leaves the file value");

        // Upgrade: the file the previous version created is read as is.
        var upgraded = File("reg-1", "aaaa-bbbb");
        Check(!Config.IsFreshConfig(upgraded), "upgrade: an existing file is never recreated");
        var upgradedSettings = Config.ResolveSettings(Config.ParseConfigText(upgraded), NoEnv, null);
        Check(upgradedSettings.RegistrationToken == "reg-1" && upgradedSettings.IdentityToken == "aaaa-bbbb",
            "upgrade: token and identity are kept");

        // A file without an identity gets one written in; the other keys stay as they were.
        var withId = Config.WithIdentity(File("reg-1", ""), "new-id");
        var withIdValues = Config.ParseConfigText(withId);
        Check(withIdValues.IdentityToken == "new-id" && withIdValues.RegistrationToken == "reg-1" &&
              withIdValues.WsUrl == Config.DefaultWsUrl && withIdValues.Debug == false,
            "identity: an empty IdentityToken is filled in, nothing else changes");
        Check(withId.IndexOf("RegistrationToken") < withId.IndexOf("IdentityToken"), "identity: key order is kept");
        Check(Config.ParseConfigText(Config.WithIdentity("{ \"RegistrationToken\": \"r\" }", "new-id")).IdentityToken == "new-id",
            "identity: a missing IdentityToken key is added");

        // Token pasted while running: a reconnect, with trimmed values.
        ConfigFileValuesCheck(upgradedSettings);

        // Half-saved and broken files while running keep the current settings.
        Config.ConfigFileValues ignored;
        Check(!Config.TryParseRunningConfig(null, out ignored), "running: a missing file is ignored");
        Check(!Config.TryParseRunningConfig("", out ignored), "running: an empty (truncated) file is ignored");
        Check(!Config.TryParseRunningConfig("{\n  \"RegistrationToken\": \"ab", out ignored), "running: a half-saved file is ignored");
        Check(!Config.TryParseRunningConfig("[1, 2]", out ignored), "running: a non-object is ignored");
        Check(!Config.IsFreshConfig("{\n  \"RegistrationToken\": \"ab"), "load: a half-saved file is never overwritten");

        // A key removed while editing keeps what is in use instead of dropping it.
        var current = Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-1")), NoEnv, null);
        var partial = Config.ResolveSettings(Config.ParseConfigText("{ \"WebSocketUrl\": \"wss://connect.takaro.io/\" }"), NoEnv, current);
        Check(partial.RegistrationToken == "reg-1" && partial.IdentityToken == "id-1" && partial.SameConnection(current),
            "running: absent keys keep the current token and identity");

        // Debug alone does not reconnect; a token or identity change does.
        var debugOnly = Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-1").Replace("false", "true")), NoEnv, current);
        Check(debugOnly.Debug && debugOnly.SameConnection(current), "running: Debug changes apply without a reconnect");
        var newToken = Config.ResolveSettings(Config.ParseConfigText(File("reg-2", "id-1")), NoEnv, current);
        Check(!newToken.SameConnection(current), "running: a new token reconnects");
        var newIdentity = Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-2")), NoEnv, current);
        Check(!newIdentity.SameConnection(current), "running: a new identity reconnects");
        var newUrl = Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-1").Replace("wss://connect.takaro.io/", "ws://127.0.0.1/")), NoEnv, current);
        Check(!newUrl.SameConnection(current), "running: a new URL reconnects");
        var envPinned = Config.ResolveSettings(Config.ParseConfigText(File("reg-2", "id-1")), Env(Config.EnvRegistrationToken, "reg-1"), current);
        Check(envPinned.RegistrationToken == "reg-1", "running: a file edit never overrides an env token");

        // Server chat name: a file key and an env var, applied without a reconnect.
        Check(!Config.IsFreshConfig("{ \"ServerChatName\": \"My Server\" }"), "chat name: a file with only ServerChatName is kept");
        Check(Config.ParseConfigText(freshText).ServerChatName == "", "chat name: a new file has an empty ServerChatName");
        Check(Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-1")), NoEnv, null).ServerChatName == "",
            "chat name: a file from an older version (no key) means no name");
        var named = Config.ResolveSettings(Config.ParseConfigText(File("reg-1", "id-1").Replace("\"Debug\"", "\"ServerChatName\": \"  My Server \",\n  \"Debug\"")), NoEnv, current);
        Check(named.ServerChatName == "My Server", "chat name: the file value is trimmed (got '" + named.ServerChatName + "')");
        Check(named.SameConnection(current), "chat name: a change applies without a reconnect");
        var envNamed = Config.ResolveSettings(Config.ParseConfigText("{ \"ServerChatName\": \"File Name\" }"), Env(Config.EnvServerChatName, " Env Name "), null);
        Check(envNamed.ServerChatName == "Env Name", "chat name: TAKARO_SERVER_CHAT_NAME wins over the file");
        var envBlank = Config.ResolveSettings(Config.ParseConfigText("{ \"ServerChatName\": \"File Name\" }"), Env(Config.EnvServerChatName, "   "), null);
        Check(envBlank.ServerChatName == "File Name", "chat name: a blank TAKARO_SERVER_CHAT_NAME counts as unset");

        // Resolution order: Takaro's override, then the setting, then no name; blank is unset.
        Check(Config.ResolveServerChatName("Override", "Config") == "Override", "chat name: senderNameOverride wins over the setting");
        Check(Config.ResolveServerChatName(" Override ", null) == "Override", "chat name: the override is trimmed");
        Check(Config.ResolveServerChatName(null, "Config") == "Config", "chat name: no override falls back to the setting");
        Check(Config.ResolveServerChatName("", "Config") == "Config", "chat name: an empty override falls back to the setting");
        Check(Config.ResolveServerChatName(" \t ", "Config") == "Config", "chat name: a whitespace override falls back to the setting");
        Check(Config.ResolveServerChatName(null, null) == null, "chat name: neither means no name");
        Check(Config.ResolveServerChatName("  ", "   ") == null, "chat name: whitespace everywhere means no name");

        // Rendering: one command for a broadcast and a whisper alike.
        string command;
        var plain = Config.ServerChatCommand(null, "hello there", out command);
        Check(command == "chat.add" && plain.Length == 3 && (int)plain[0] == 2 && (int)plain[1] == 0 && (string)plain[2] == "hello there",
            "render: no name sends chat.add 2 0 <text>, as before");
        var named2 = Config.ServerChatCommand("My Server", "hello", out command);
        Check(command == "chat.add2", "render: a name sends chat.add2 (got '" + command + "')");
        Check(named2.Length == 6 && (int)named2[0] == 2 && (int)named2[1] == 0 && (string)named2[2] == "hello" &&
              (string)named2[3] == "My Server" && (string)named2[4] == Config.ServerChatNameColor && (float)named2[5] == 1f,
            "render: chat.add2 on the Server channel, text, then the name as the sender");
        var tagged = (string)Config.ServerChatCommand("<color=red>X</color>", "hi", out command)[3];
        Check(!tagged.Contains("<color") && tagged.Replace("\u200b", "") == "<color=red>X</color>", "render: tags in a name are escaped (got '" + tagged + "')");
        Check((string)Config.ServerChatCommand("A", "<b>bold</b>", out command)[2] == "<b>bold</b>", "render: the message itself is left as Takaro sent it");

        // Identify errors: never the raw object, never a JWT.
        const string jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.c2lnbmF0dXJl";
        var leaky = Newtonsoft.Json.Linq.JObject.Parse("{\"name\":\"Conflict\",\"message\":\"server exists\",\"status\":409," +
            "\"config\":{\"headers\":{\"x-takaro-token\":\"" + jwt + "\"}}}");
        var errorText = Config.IdentifyErrorText(leaky);
        Check(errorText == "Conflict: server exists: HTTP 409", "identify error: name, message and status only (got '" + errorText + "')");
        Check(!Config.IdentifyErrorText(Newtonsoft.Json.Linq.JObject.Parse("{\"config\":{\"t\":\"" + jwt + "\"}}")).Contains("eyJ"),
            "identify error: an object without name/message never dumps itself");
        Check(!Config.IdentifyErrorText(new Newtonsoft.Json.Linq.JValue("bad token, header x-takaro-token: " + jwt)).Contains(jwt),
            "identify error: a JWT inside a string is redacted");
        Check(Config.IdentifyErrorText(new Newtonsoft.Json.Linq.JValue("Invalid registrationToken provided")) == "Invalid registrationToken provided",
            "identify error: a plain message is kept");

        Console.WriteLine("checks: " + _checks + ", failures: " + _failures);
        return _failures == 0 ? 0 : 1;
    }

    private static void ConfigFileValuesCheck(Config.ConnectorSettings before)
    {
        var pasted = Config.ParseConfigText("{ \"RegistrationToken\": \"  reg-2\\n\", \"IdentityToken\": \"aaaa-bbbb\", \"Debug\": \"true\" }");
        var after = Config.ResolveSettings(pasted, NoEnv, before);
        Check(after.RegistrationToken == "reg-2", "paste: surrounding whitespace is trimmed");
        Check(after.Debug, "paste: Debug as the string \"true\" is accepted");
        Check(after.WsUrl == Config.DefaultWsUrl, "paste: a missing URL is the default");
        Check(!after.SameConnection(before), "paste: a pasted token reconnects");
    }
}
