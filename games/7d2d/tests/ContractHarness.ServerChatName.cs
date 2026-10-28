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
        }
        finally
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
    }
}
