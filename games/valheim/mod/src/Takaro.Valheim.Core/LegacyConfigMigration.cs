namespace Takaro.Valheim.Core;

/// <summary>
/// Carries the settings of an older connector version into the one config file, so an upgrade
/// keeps the settings the server ran with.
///
/// 4.2.x read its settings from BepInEx/plugins/TakaroValheim/takaro.cfg and kept only the
/// registration token and identity in BepInEx/config/com.takaro.valheim.cfg; every other key
/// there is a default, written as text (serverName = Valheim Server, takaroWsUrl = the
/// takaro.io URL). Read as-is, those defaults would replace the settings in takaro.cfg, and
/// "Valheim Server" would replace the name 4.2.x computed. Releases up to 4.1 kept everything in
/// com.takaro.valheim.cfg (written by BepInEx) and used serverName as the identity while
/// identityToken was empty.
///
/// Free of file access so tests can cover it: the store reads and writes the files.
/// </summary>
public static class LegacyConfigMigration
{
    /// <summary>The takaro.cfg 4.2.x shipped in the plugin folder.</summary>
    public const string LegacyUserFileName = "takaro.cfg";

    /// <summary>What a migrated takaro.cfg is renamed to, so it is read once and never mistaken for the config.</summary>
    public const string MigratedSuffix = ".migrated";

    /// <summary>The first line of the com.takaro.valheim.cfg that 4.2.x created.</summary>
    public const string Release42Marker = "## Written by the Takaro Valheim connector: the token and identity it uses";

    /// <summary>The first line BepInEx's config writer put in the file for releases up to 4.1.</summary>
    public const string Release41Marker = "## Settings file was created by plugin Takaro Valheim";

    /// <summary>The serverName default up to 4.2.x; there it meant "the server's own name plus the start of the identity".</summary>
    public const string LegacyDefaultServerName = "Valheim Server";

    /// <summary>True for a com.takaro.valheim.cfg that an older connector version wrote.</summary>
    public static bool IsLegacy(string? configText) =>
        configText is not null && (StartsWith(configText, Release42Marker) || StartsWith(configText, Release41Marker));

    /// <summary>
    /// The settings the older version used, as a full key set for <see cref="ConnectorConfigFiles.NewFile(IReadOnlyDictionary{string, string})"/>.
    /// Null when there is nothing to migrate: the config is in the current format, or neither text
    /// holds a [Takaro] section. A key takaro.cfg sets wins, else the value in the config file,
    /// else the default, as in 4.2.x. An empty identityToken stays empty (the store then writes a
    /// new one); an empty serverName is filled by the store with the server's own name plus the
    /// start of the identity, which is the name 4.2.x announced.
    /// </summary>
    public static Dictionary<string, string>? Resolve(string? configText, string? legacyUserText)
    {
        if (configText is not null && !IsLegacy(configText))
        {
            return null;
        }

        var saved = TryParse(configText);
        var user = TryParse(legacyUserText);
        if (saved is null && user is null)
        {
            return null;
        }

        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in ConnectorConfigFiles.Defaults)
        {
            values[entry.Key] = Value(user, entry.Key) ?? Value(saved, entry.Key) ?? entry.Value;
        }

        var identity = Value(user, ConnectorConfigFiles.IdentityTokenKey) ?? Value(saved, ConnectorConfigFiles.IdentityTokenKey);
        if (identity is null
            && configText is not null
            && StartsWith(configText, Release41Marker)
            && Value(saved, ConnectorConfigFiles.RegistrationTokenKey) is not null)
        {
            // Up to 4.1 Takaro knew a server with a token but no identityToken by its serverName.
            identity = Value(saved, ConnectorConfigFiles.ServerNameKey) ?? LegacyDefaultServerName;
        }

        values[ConnectorConfigFiles.IdentityTokenKey] = identity ?? "";
        if (values[ConnectorConfigFiles.ServerNameKey] == LegacyDefaultServerName && identity != LegacyDefaultServerName)
        {
            values[ConnectorConfigFiles.ServerNameKey] = "";
        }

        return values;
    }

    private static Dictionary<string, string>? TryParse(string? text)
    {
        if (text is null)
        {
            return null;
        }

        try
        {
            return ConnectorConfigFiles.Parse(text);
        }
        catch (FormatException)
        {
            return null;
        }
    }

    private static bool StartsWith(string text, string marker) =>
        text.TrimStart('﻿', ' ', '\t', '\r', '\n').StartsWith(marker, StringComparison.Ordinal);

    private static string? Value(IReadOnlyDictionary<string, string>? values, string key) =>
        values is not null && values.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value)
            ? value.Trim()
            : null;
}
