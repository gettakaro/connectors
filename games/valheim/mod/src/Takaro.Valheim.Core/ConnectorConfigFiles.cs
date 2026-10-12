using System.Text;

namespace Takaro.Valheim.Core;

/// <summary>
/// Reading and writing the connector's one config file, BepInEx/config/com.takaro.valheim.cfg,
/// free of game types so tests can cover it. A BepInEx-style INI with a [Takaro] section.
/// </summary>
public static class ConnectorConfigFiles
{
    public const string Section = "Takaro";
    public const string FileName = "com.takaro.valheim.cfg";
    public const string DefaultServerName = "Valheim Server";
    public const string DefaultTakaroWsUrl = "wss://connect.takaro.io/";

    public const string RegistrationTokenKey = "registrationToken";
    public const string IdentityTokenKey = "identityToken";
    public const string ServerNameKey = "serverName";
    public const string TakaroWsUrlKey = "takaroWsUrl";

    public static readonly IReadOnlyDictionary<string, string> Defaults = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [RegistrationTokenKey] = "",
        [ServerNameKey] = "",
        [IdentityTokenKey] = "",
        [TakaroWsUrlKey] = DefaultTakaroWsUrl,
        ["logLevel"] = "Information",
        ["enableLogEvents"] = "true",
        ["commandAllowlistExact"] = "help",
        ["commandAllowlistPrefixes"] = "",
        ["chatSenderName"] = ConnectorConfig.DefaultChatSenderName
    };

    /// <summary>The settings a change of which needs a new connection (the identify carries them).</summary>
    public static readonly IReadOnlyList<string> ConnectionKeys = new[]
    {
        TakaroWsUrlKey, RegistrationTokenKey, IdentityTokenKey, ServerNameKey
    };

    private static readonly IReadOnlyDictionary<string, string> Comments = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [RegistrationTokenKey] = "Takaro registration token: paste it here and save; the connector connects within a few seconds.",
        [ServerNameKey] = "The server name shown in Takaro; must be unique in your Takaro domain. Empty: the server's own name plus the start of identityToken.",
        [IdentityTokenKey] = "Identifies this server in Takaro. Filled in by the connector; keep it.",
        [TakaroWsUrlKey] = "Takaro connector WebSocket URL; keep the default.",
        ["logLevel"] = "Connector log level.",
        ["enableLogEvents"] = "Forward connector log events to Takaro.",
        ["commandAllowlistExact"] = "Semicolon-separated exact console commands allowed for executeConsoleCommand.",
        ["commandAllowlistPrefixes"] = "Semicolon-separated console command prefixes allowed for executeConsoleCommand.",
        ["chatSenderName"] = "Name shown in game chat for Takaro messages (unless Takaro sends its own sender name)."
    };

    /// <summary>
    /// The name sent when serverName is empty: the dedicated server's own name (or the default)
    /// plus the start of the identity, so two servers never announce the same name (Takaro
    /// answers a duplicate name with a 409).
    /// </summary>
    public static string UniqueServerName(string identity, string? hostServerName) =>
        (string.IsNullOrWhiteSpace(hostServerName) ? DefaultServerName : hostServerName!.Trim())
        + " (" + new string(identity.Where(char.IsLetterOrDigit).Take(8).ToArray()) + ")";

    /// <summary>A new config file: every key with a short comment, an empty token and the given identity and name.</summary>
    public static string NewFile(string identity, string serverName)
    {
        var text = new StringBuilder()
            .Append("## Takaro Valheim connector settings. Saving this file applies the token, identity,\n")
            .Append("## serverName and takaroWsUrl within a few seconds; other settings need a server restart.\n\n")
            .Append('[').Append(Section).Append("]\n");
        foreach (var entry in Defaults)
        {
            var value = entry.Key switch
            {
                IdentityTokenKey => identity,
                ServerNameKey => serverName,
                _ => entry.Value
            };
            text.Append("\n## ").Append(Comments[entry.Key]).Append('\n').Append(Line(entry.Key, value)).Append('\n');
        }

        return text.ToString();
    }

    /// <summary>
    /// The [Takaro] settings in a BepInEx-style config text. Throws FormatException when the text
    /// has no [Takaro] section, which is how an empty or half-written file looks.
    /// </summary>
    public static Dictionary<string, string> Parse(string text)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        var inSection = false;
        var sawSection = false;
        foreach (var rawLine in SplitLines(text))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            if (line[0] == '[' && line[line.Length - 1] == ']')
            {
                inSection = string.Equals(line.Substring(1, line.Length - 2).Trim(), Section, StringComparison.Ordinal);
                sawSection |= inSection;
                continue;
            }

            var equals = line.IndexOf('=');
            if (!inSection || equals <= 0)
            {
                continue;
            }

            values[line.Substring(0, equals).Trim()] = line.Substring(equals + 1).Trim();
        }

        if (!sawSection)
        {
            throw new FormatException("no [Takaro] section");
        }

        return values;
    }

    /// <summary>
    /// Sets keys of the [Takaro] section in an existing config text, keeping every other line,
    /// comment and unknown key; a key that is missing is added at the end of the section.
    /// </summary>
    public static string SetValues(string text, IReadOnlyDictionary<string, string> updates)
    {
        var newline = text.IndexOf("\r\n", StringComparison.Ordinal) >= 0 ? "\r\n" : "\n";
        var lines = SplitLines(text).ToList();
        if (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
        {
            lines.RemoveAt(lines.Count - 1);
        }

        var pending = new Dictionary<string, string>(updates.ToDictionary(e => e.Key, e => e.Value), StringComparer.Ordinal);
        var inSection = false;
        var sectionEnd = -1;
        for (var i = 0; i < lines.Count; i++)
        {
            var line = lines[i].Trim();
            if (line.Length > 1 && line[0] == '[' && line[line.Length - 1] == ']')
            {
                inSection = string.Equals(line.Substring(1, line.Length - 2).Trim(), Section, StringComparison.Ordinal);
                if (inSection)
                {
                    sectionEnd = i + 1;
                }

                continue;
            }

            if (!inSection)
            {
                continue;
            }

            if (line.Length > 0 && line[0] != '#' && line[0] != ';')
            {
                sectionEnd = i + 1;
            }

            var equals = line.IndexOf('=');
            if (equals <= 0 || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            // Every occurrence: Parse takes the last one, so a stale duplicate must not survive.
            var key = line.Substring(0, equals).Trim();
            if (updates.TryGetValue(key, out var value))
            {
                lines[i] = Line(key, value);
                pending.Remove(key);
            }
        }

        if (pending.Count > 0)
        {
            var added = pending.Select(entry => Line(entry.Key, entry.Value)).ToList();
            if (sectionEnd < 0)
            {
                lines.Add("");
                lines.Add("[" + Section + "]");
                lines.AddRange(added);
            }
            else
            {
                lines.InsertRange(sectionEnd, added);
            }
        }

        return string.Join(newline, lines) + newline;
    }

    /// <summary>
    /// Writes through a temporary file, so a reader polling the file never sees half of it. The
    /// temporary file starts as a copy of an existing file, so the file keeps its mode.
    /// </summary>
    public static void WriteAtomically(string path, string text)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var temporary = path + ".tmp";
        var exists = File.Exists(path);
        if (exists)
        {
            File.Copy(path, temporary, overwrite: true);
        }

        File.WriteAllText(temporary, text, new UTF8Encoding(false));
        if (exists)
        {
            File.Replace(temporary, path, null);
        }
        else
        {
            File.Move(temporary, path);
        }
    }

    private static string Line(string key, string value) =>
        Clean(value).Length == 0 ? key + " =" : key + " = " + Clean(value);

    // A token pasted with a stray line break must not split into a second line.
    private static string Clean(string value) => value.Replace("\r", "").Replace("\n", "").Trim();

    private static IEnumerable<string> SplitLines(string text) => text.Replace("\r\n", "\n").Split('\n');
}
