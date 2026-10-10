using System.Reflection;
using System.Text;

namespace Takaro.Valheim.Core;

/// <summary>Where the identity in use came from.</summary>
public enum IdentitySource
{
    /// <summary>identityToken in the plugin folder's takaro.cfg.</summary>
    UserFile,

    /// <summary>identityToken in BepInEx/config/com.takaro.valheim.cfg.</summary>
    SavedFile,

    /// <summary>
    /// The serverName of an install that ran a release up to 4.1, which identified with its
    /// serverName whenever identityToken was empty.
    /// </summary>
    LegacyServerName,

    /// <summary>A new identity, for an install that never had one.</summary>
    Generated
}

public sealed class ConfigResolution
{
    public ConfigResolution(IReadOnlyDictionary<string, string> values, IdentitySource identitySource)
    {
        Values = values;
        IdentitySource = identitySource;
    }

    public IReadOnlyDictionary<string, string> Values { get; }

    public IdentitySource IdentitySource { get; }
}

/// <summary>
/// Reading, merging and writing the two config files, free of game types so tests can cover it.
///
/// The user file (BepInEx/plugins/TakaroValheim/takaro.cfg) ships in the release zip and is
/// the file people edit. The saved file (BepInEx/config/com.takaro.valheim.cfg) is where
/// releases up to 4.1 kept everything; the connector now writes the token and identity in use
/// there, so replacing the plugin folder on an upgrade keeps both.
/// </summary>
public static class ConnectorConfigFiles
{
    public const string Section = "Takaro";
    public const string UserFileName = "takaro.cfg";
    public const string SavedFileName = "com.takaro.valheim.cfg";
    public const string DefaultServerName = "Valheim Server";
    public const string DefaultTakaroWsUrl = "wss://connect.takaro.io/";

    public const string RegistrationTokenKey = "registrationToken";
    public const string IdentityTokenKey = "identityToken";
    public const string ServerNameKey = "serverName";
    public const string TakaroWsUrlKey = "takaroWsUrl";

    /// <summary>
    /// The first line BepInEx's own config writer puts in the file. Releases up to 4.1 bound their
    /// settings through BepInEx, so a saved file that starts like this was written by one of them.
    /// </summary>
    public const string LegacyWriterMarker = "## Settings file was created by plugin Takaro Valheim";

    public static readonly IReadOnlyDictionary<string, string> Defaults = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [RegistrationTokenKey] = "",
        [ServerNameKey] = DefaultServerName,
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

    private const string SavedFileHeader =
        "## Written by the Takaro Valheim connector: the token and identity it uses, kept outside\n"
        + "## BepInEx/plugins so that upgrading the connector keeps them. Edit\n"
        + "## BepInEx/plugins/TakaroValheim/takaro.cfg instead; a registrationToken set there takes\n"
        + "## precedence over this file.\n";

    /// <summary>The takaro.cfg that ships in the release zip, embedded so a missing one can be recreated.</summary>
    public static string ShippedUserFile()
    {
        using var stream = typeof(ConnectorConfigFiles).GetTypeInfo().Assembly
            .GetManifestResourceStream("Takaro.Valheim.Core.takaro.cfg")
            ?? throw new InvalidOperationException("The shipped takaro.cfg is not embedded in Takaro.Valheim.Core.");
        using var reader = new StreamReader(stream, Encoding.UTF8);
        return reader.ReadToEnd();
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
    /// The settings to use: a key the user file sets wins, else the saved file, else the default.
    /// The shipped user file leaves every key but the two tokens commented out, so an upgrade
    /// that brings a fresh copy never overrides what the saved file holds, while a value written
    /// into the user file always applies, even when it equals the default. An identity neither file holds is the serverName for
    /// an install a release up to 4.1 ran with a token (that is the identity Takaro knows it
    /// by), else a new one from <paramref name="newIdentity"/>.
    /// </summary>
    public static ConfigResolution Resolve(
        IReadOnlyDictionary<string, string>? user,
        IReadOnlyDictionary<string, string>? saved,
        bool savedWrittenByLegacyRelease,
        Func<string> newIdentity,
        string? hostServerName = null)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in Defaults)
        {
            values[entry.Key] = Value(user, entry.Key) ?? Value(saved, entry.Key) ?? entry.Value;
        }

        values[RegistrationTokenKey] = Value(user, RegistrationTokenKey) ?? Value(saved, RegistrationTokenKey) ?? "";

        IdentitySource source;
        string identity;
        if (Value(user, IdentityTokenKey) is { } userIdentity)
        {
            identity = userIdentity;
            source = IdentitySource.UserFile;
        }
        else if (Value(saved, IdentityTokenKey) is { } savedIdentity)
        {
            identity = savedIdentity;
            source = IdentitySource.SavedFile;
        }
        else if (savedWrittenByLegacyRelease && Value(saved, RegistrationTokenKey) is not null)
        {
            identity = Value(saved, ServerNameKey) ?? DefaultServerName;
            source = IdentitySource.LegacyServerName;
        }
        else
        {
            identity = newIdentity();
            source = IdentitySource.Generated;
        }

        values[IdentityTokenKey] = identity;

        // Takaro refuses a second game server with a name the domain already has (409), so an
        // unset serverName must not announce a shared name: it becomes the name the dedicated
        // server runs with (-name, else the default) plus the start of the identity. An install
        // whose identity is the default name (releases up to 4.1) keeps that name.
        if (values[ServerNameKey] == DefaultServerName && identity != DefaultServerName)
        {
            var baseName = string.IsNullOrWhiteSpace(hostServerName) ? DefaultServerName : hostServerName!.Trim();
            values[ServerNameKey] = baseName + " (" + new string(identity.Where(char.IsLetterOrDigit).Take(8).ToArray()) + ")";
        }

        return new ConfigResolution(values, source);
    }

    /// <summary>
    /// The values the saved file must hold: the token and identity in use. Its other settings
    /// are its own, so a setting made in the user file is not copied there, and setting it back to
    /// the default in the user file takes effect instead of leaving the old value behind.
    /// </summary>
    public static Dictionary<string, string> SavedFileUpdates(
        IReadOnlyDictionary<string, string> resolved,
        IReadOnlyDictionary<string, string>? saved)
    {
        var updates = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var key in new[] { RegistrationTokenKey, IdentityTokenKey })
        {
            var inUse = resolved[key];
            if ((Value(saved, key) ?? "") != inUse)
            {
                updates[key] = inUse;
            }
        }

        return updates;
    }

    /// <summary>A new saved file: every default, with the token and identity in use.</summary>
    public static string NewSavedFile(IReadOnlyDictionary<string, string> resolved)
    {
        var text = new StringBuilder(SavedFileHeader).Append('\n').Append('[').Append(Section).Append("]\n");
        foreach (var entry in Defaults)
        {
            var value = entry.Key is RegistrationTokenKey or IdentityTokenKey ? resolved[entry.Key] : entry.Value;
            text.Append('\n').Append(entry.Key).Append(" = ").Append(Clean(value)).Append('\n');
        }

        return text.ToString();
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

    /// <summary>Writes through a temporary file, so a reader polling the file never sees half of it.</summary>
    public static void WriteAtomically(string path, string text)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var temporary = path + ".tmp";
        File.WriteAllText(temporary, text, new UTF8Encoding(false));
        if (File.Exists(path))
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

    private static string? Value(IReadOnlyDictionary<string, string>? values, string key) =>
        values is not null && values.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value)
            ? value.Trim()
            : null;
}
