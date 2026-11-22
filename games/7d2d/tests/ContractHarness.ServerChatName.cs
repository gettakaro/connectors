using System;
using System.IO;
using Takaro.Config;
using Takaro.Services;

// The sender name on chat the connector sends (gettakaro/connectors#669): Takaro's
// senderNameOverride, then Config.xml's ServerChatName, then the game's own "Server".
public static partial class ContractHarness
{
    private static void FamilyServerChatNameResolution()
    {
        Equal("Takaro", ServerChatName.Resolve("Takaro", "Config"), "the override wins");
        Equal("Config", ServerChatName.Resolve(null, "Config"), "no override uses the config");
        Equal("Config", ServerChatName.Resolve("", "Config"), "an empty override is unset");
        Equal("Config", ServerChatName.Resolve(" \t ", "Config"), "a whitespace override is unset");
        Equal("Takaro", ServerChatName.Resolve("  Takaro ", "Config"), "the override is trimmed");
        Equal("Config", ServerChatName.Resolve(null, "  Config\n"), "the config is trimmed");
        Equal(null, ServerChatName.Resolve(null, null), "neither set means no name");
        Equal(null, ServerChatName.Resolve("", ""), "both empty means no name");
        Equal(null, ServerChatName.Resolve("  ", " "), "both whitespace means no name");
    }

    private static void FamilyServerChatNameRendering()
    {
        Equal("hello", ServerChatName.Render(null, "hello"), "no name keeps the text as it is");
        Equal(
            "[ff0000]red[-] text",
            ServerChatName.Render(null, "[ff0000]red[-] text"),
            "no name keeps BBCode as it is"
        );
        Equal("X: hello", ServerChatName.Render("X", "hello"), "a name goes in front");
        Equal(
            "My Server: hi: there",
            ServerChatName.Render("My Server", "hi: there"),
            "a name with spaces and a colon in the text"
        );

        // The echo guard matches the game's log line for the rendered text, so a
        // named global message never comes back as a log event.
        var guard = new ServerMessageEchoGuard();
        DateTimeOffset now = DateTimeOffset.Parse("2026-10-28T20:00:00Z");
        string rendered = ServerChatName.Render(
            ServerChatName.Resolve(null, "Takaro Bot"),
            "welcome"
        );
        guard.Record(rendered, now);
        True(
            guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): Takaro Bot: welcome",
                now.AddSeconds(1)
            ),
            "the named global message's log echo is suppressed"
        );
    }

    private static void FamilyServerChatNameConfig()
    {
        string dir = Path.Combine(Path.GetTempPath(), "takaro-chatname-" + Guid.NewGuid());
        string path = Path.Combine(dir, "Takaro", "Config.xml");
        try
        {
            ConfigValues fresh = ConfigFiles.Load(path, () => "identity", out _, out _);
            Equal("", fresh.ServerChatName, "a new config has no chat name");
            True(fresh.ReportCharacters, "a new config reports characters");
            True(
                File.ReadAllText(path).Contains("<ReportCharacters>true</ReportCharacters>"),
                "a new config spells out the characters key"
            );
            True(
                File.ReadAllText(path).Contains("<Takaro version=\"2\">"),
                "a new config carries the layout version"
            );
            File.WriteAllText(
                path,
                File.ReadAllText(path)
                    .Replace(
                        "<ReportCharacters>true</ReportCharacters>",
                        "<ReportCharacters>false</ReportCharacters>"
                    )
            );
            ConfigValues off = ConfigFiles.Load(path, () => "unused", out bool wroteOff, out _);
            True(!off.ReportCharacters, "turning characters off is honoured");
            True(!wroteOff && !off.Upgraded, "an admin's false is never rewritten");
            File.WriteAllText(
                path,
                File.ReadAllText(path)
                    .Replace(
                        "<ReportCharacters>false</ReportCharacters>",
                        "<ReportCharacters>true</ReportCharacters>"
                    )
            );
            True(
                File.ReadAllText(path).Contains("<ServerChatName></ServerChatName>"),
                "a new config spells out the chat name key"
            );

            File.WriteAllText(
                path,
                File.ReadAllText(path)
                    .Replace(
                        "<ServerChatName></ServerChatName>",
                        "<ServerChatName>  Takaro Bot \n</ServerChatName>"
                    )
            );
            ConfigValues named = ConfigFiles.Load(path, () => "unused", out bool wrote, out _);
            Equal("Takaro Bot", named.ServerChatName, "the chat name is read and trimmed");
            True(!wrote, "setting the chat name does not make the mod write the file");
            True(named.SameConnection(fresh), "the chat name is not a connection change");

            File.WriteAllText(
                path,
                "<Takaro><WebSocket><IdentityToken>id</IdentityToken></WebSocket></Takaro>"
            );
            Equal(
                "",
                ConfigFiles.Read(path).ServerChatName,
                "a file from an older release has no chat name"
            );
            True(
                ConfigFiles.Read(path).ReportCharacters,
                "a file from before 0.9 reports characters by default"
            );
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }

    // 0.9.0 to 0.10.2 wrote every new Config.xml with ReportCharacters false
    // (their default) and no layout version.
    private const string OldReleaseConfig =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        + "<!--\n"
        + "  ReportCharacters tells Takaro about each life on a Permanent Death server.\n"
        + "  Turn it on only when your Takaro supports Characters, then restart the server.\n"
        + "-->\n"
        + "<Takaro>\n"
        + "  <WebSocket>\n"
        + "    <Url>wss://example.invalid/</Url>\n"
        + "    <RegistrationToken>reg</RegistrationToken>\n"
        + "    <IdentityToken>{IDENTITY}</IdentityToken>\n"
        + "    <Enabled>true</Enabled>\n"
        + "    <ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>\n"
        + "  </WebSocket>\n"
        + "  <Chat>\n"
        + "    <ServerChatName>Bot</ServerChatName>\n"
        + "  </Chat>\n"
        + "  <Players>\n"
        + "    <ReportCharacters>false</ReportCharacters>\n"
        + "  </Players>\n"
        + "</Takaro>\n";

    private static void FamilyReportCharactersUpgrade()
    {
        string dir = Path.Combine(Path.GetTempPath(), "takaro-charsup-" + Guid.NewGuid());
        string path = Path.Combine(dir, "Takaro", "Config.xml");
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        try
        {
            File.WriteAllText(path, OldReleaseConfig.Replace("{IDENTITY}", "id-1"));
            ConfigValues up = ConfigFiles.Load(
                path,
                () => "unused",
                out bool wrote,
                out Exception err
            );
            True(err == null, "the upgrade writes the file");
            True(wrote && up.Upgraded, "a file from an older release is upgraded once");
            True(up.ReportCharacters, "the old written default false turns characters on");
            string text = File.ReadAllText(path);
            True(
                File.ReadAllBytes(path)[0] == (byte)'<',
                "the rewritten file gets no byte order mark"
            );
            True(text.Contains("<Takaro version=\"2\">"), "the upgrade stamps the layout version");
            True(
                text.Contains("<ReportCharacters>true</ReportCharacters>"),
                "the upgraded file shows the value the mod uses"
            );
            True(!text.Contains("Turn it on only when"), "the old hint is replaced");
            True(text.Contains("set it to false to keep lives out"), "the new hint is written");
            True(
                ConfigFiles.Header.EndsWith(ConfigFiles.CharactersHint),
                "the upgrade writes the same hint a new file gets"
            );
            Equal("id-1", up.IdentityToken, "the identity is kept");
            Equal("reg", up.RegistrationToken, "the registration token is kept");
            Equal("Bot", up.ServerChatName, "the chat name is kept");
            Equal(30, up.ReconnectIntervalSeconds, "the reconnect interval is kept");
            True(
                text.Contains("<ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>"),
                "other settings stay as written"
            );

            ConfigValues again = ConfigFiles.Load(path, () => "unused", out bool wroteAgain, out _);
            True(!wroteAgain && !again.Upgraded, "an upgraded file is not written again");

            // The admin turns it off after the upgrade: that choice stands.
            File.WriteAllText(
                path,
                text.Replace(
                    "<ReportCharacters>true</ReportCharacters>",
                    "<ReportCharacters>false</ReportCharacters>"
                )
            );
            ConfigValues chosen = ConfigFiles.Load(
                path,
                () => "unused",
                out bool wroteChosen,
                out _
            );
            True(
                !chosen.ReportCharacters && !wroteChosen,
                "a false set after the upgrade is honoured"
            );

            // An old file the admin had turned on stays on.
            File.WriteAllText(
                path,
                OldReleaseConfig
                    .Replace("{IDENTITY}", "id-2")
                    .Replace(
                        "<ReportCharacters>false</ReportCharacters>",
                        "<ReportCharacters>true</ReportCharacters>"
                    )
            );
            ConfigValues on = ConfigFiles.Load(path, () => "unused", out _, out _);
            True(on.ReportCharacters && on.Upgraded, "an old true stays true and is stamped");

            // An old file without an identity gets both in one write.
            File.WriteAllText(path, OldReleaseConfig.Replace("{IDENTITY}", ""));
            ConfigValues both = ConfigFiles.Load(path, () => "new-id", out _, out _);
            Equal(
                "new-id",
                both.IdentityToken,
                "a missing identity is generated during the upgrade"
            );
            True(
                File.ReadAllText(path).Contains("<IdentityToken>new-id</IdentityToken>"),
                "and written"
            );
            True(both.ReportCharacters, "and characters are on");

            // A file from before 0.9 has no Players section: it gets one.
            File.WriteAllText(
                path,
                "<Takaro><WebSocket><IdentityToken>id-3</IdentityToken></WebSocket></Takaro>"
            );
            ConfigValues pre = ConfigFiles.Load(path, () => "unused", out _, out _);
            True(pre.ReportCharacters && pre.Upgraded, "a pre-0.9 file reports characters");
            True(
                File.ReadAllText(path).Contains("<ReportCharacters>true</ReportCharacters>"),
                "a pre-0.9 file gets the setting written out"
            );
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }
}
