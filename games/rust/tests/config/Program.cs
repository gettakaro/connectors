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
        var fresh = Config.FirstConfig(NoEnv, () => "11111111-2222-3333-4444-555555555555");
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

        // Existing environment install upgraded to this version: it keeps the identity it
        // was sending, including the empty one, and never gets a generated one.
        var generated = false;
        var envOnly = Config.FirstConfig(Env(Config.EnvRegistrationToken, "reg"), () => { generated = true; return "x"; });
        Check(!generated && envOnly.IdentityToken == "", "env install: no identity is generated when the env token is set");
        var envOnlySettings = Config.ResolveSettings(Config.ParseConfigText(Config.RenderConfigText(envOnly)),
            Env(Config.EnvRegistrationToken, "reg"), null);
        Check(envOnlySettings.RegistrationToken == "reg" && envOnlySettings.IdentityToken == "",
            "env install: sends the env token and the empty identity, as before");
        var envBoth = Config.FirstConfig(Env(Config.EnvIdentityToken, "my-rust-1"), () => { generated = true; return "x"; });
        Check(!generated && envBoth.IdentityToken == "", "env install: no identity is generated when the env identity is set");
        var envBothSettings = Config.ResolveSettings(envBoth,
            Env(Config.EnvRegistrationToken, "reg", Config.EnvIdentityToken, "my-rust-1", Config.EnvWsUrl, "ws://x/", Config.EnvDebug, "TRUE"), null);
        Check(envBothSettings.IdentityToken == "my-rust-1" && envBothSettings.IdentityFromEnv, "env: TAKARO_IDENTITY_TOKEN wins");
        Check(envBothSettings.WsUrl == "ws://x/", "env: TAKARO_WS_URL wins");
        Check(envBothSettings.Debug, "env: TAKARO_DEBUG=TRUE is true, as before");

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
        var blankIdentity = File("reg-1", "");
        Check(!Config.IsFreshConfig(blankIdentity) &&
              Config.ResolveSettings(Config.ParseConfigText(blankIdentity), NoEnv, null).IdentityToken == "",
            "upgrade: an existing empty identity stays empty (no new server record)");

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
