namespace Takaro.Valheim.Core;

/// <summary>What a config check found.</summary>
public sealed class ConfigChange
{
    public ConfigChange(ConnectorConfig config, bool connectionChanged, IReadOnlyList<string> restartKeys)
    {
        Config = config;
        ConnectionChanged = connectionChanged;
        RestartKeys = restartKeys;
    }

    public ConnectorConfig Config { get; }

    /// <summary>The URL, a token or serverName changed: the connection must be made afresh.</summary>
    public bool ConnectionChanged { get; }

    /// <summary>Changed settings that only a server restart applies.</summary>
    public IReadOnlyList<string> RestartKeys { get; }
}

/// <summary>
/// Loads the connector settings from the user file and the saved file, writes the identity and
/// token back where they must be kept, and tells a caller polling it when the files changed.
/// Thread-safe: the plugin calls <see cref="Load"/> once on the main thread and
/// <see cref="CheckForChanges"/> from a timer.
/// </summary>
public sealed class ConnectorConfigStore
{
    private readonly object gate = new();
    private readonly TimeSpan settleDelay;
    private readonly Action<string> info;
    private readonly Action<string> warn;
    private readonly Func<string> newIdentity;
    private IReadOnlyDictionary<string, string>? current;
    private string? userStamp;
    private string? savedStamp;
    private string? lastProblem;
    private string? userToken;
    private readonly string? hostServerName;

    public ConnectorConfigStore(
        string userPath,
        string savedPath,
        Action<string>? info = null,
        Action<string>? warn = null,
        Func<string>? newIdentity = null,
        TimeSpan? settleDelay = null,
        string? hostServerName = null)
    {
        this.hostServerName = hostServerName;
        this.settleDelay = settleDelay ?? TimeSpan.FromSeconds(1);
        UserPath = userPath;
        SavedPath = savedPath;
        this.info = info ?? (_ => { });
        this.warn = warn ?? (_ => { });
        this.newIdentity = newIdentity ?? (() => Guid.NewGuid().ToString());
    }

    /// <summary>The file people edit: BepInEx/plugins/TakaroValheim/takaro.cfg.</summary>
    public string UserPath { get; }

    /// <summary>The connector-kept copy: BepInEx/config/com.takaro.valheim.cfg.</summary>
    public string SavedPath { get; }

    /// <summary>
    /// The first load. Returns false, with the reason, only when the settings are invalid (for
    /// example an over-long chatSenderName); an unreadable file counts as empty.
    /// </summary>
    public bool Load(out ConnectorConfig? config, out string error)
    {
        lock (gate)
        {
            var change = Reload(firstLoad: true, out error);
            config = change?.Config;
            return config is not null;
        }
    }

    /// <summary>
    /// Re-reads both files when their text changed. Returns null when nothing changed or a file
    /// could not be read or parsed (the current settings stay, and the next check retries).
    /// </summary>
    public ConfigChange? CheckForChanges()
    {
        lock (gate)
        {
            if (current is null)
            {
                return null;
            }

            var readable = ReadText(UserPath, out var userText) & ReadText(SavedPath, out var savedText);
            if (readable && userText == userStamp && savedText == savedStamp)
            {
                return null;
            }

            // An editor saving in place can be caught half-way with a truncated but readable
            // token; only apply a text that stays the same for a moment.
            Thread.Sleep(settleDelay);
            if (!ReadText(UserPath, out var userAgain) || !ReadText(SavedPath, out var savedAgain)
                || userAgain != userText || savedAgain != savedText)
            {
                return null;
            }

            return Reload(firstLoad: false, out _);
        }
    }

    private ConfigChange? Reload(bool firstLoad, out string error)
    {
        error = "";
        var userReadable = ReadText(UserPath, out var userText);
        var savedReadable = ReadText(SavedPath, out var savedText);
        if (!firstLoad && (!userReadable || !savedReadable))
        {
            Problem("could not read the Takaro config files; keeping the current settings and trying again in a few seconds");
            return null;
        }

        // An unreadable file at startup counts as empty and is never overwritten.
        var user = ParseOrNull(userText, UserPath, firstLoad, out var userUnreadable);
        var saved = ParseOrNull(savedText, SavedPath, firstLoad, out var savedUnreadable);
        userUnreadable |= !userReadable;
        savedUnreadable |= !savedReadable;
        if (!firstLoad && (userUnreadable || savedUnreadable))
        {
            return null;
        }

        var legacy = savedText is not null
            && savedText.TrimStart().StartsWith(ConnectorConfigFiles.LegacyWriterMarker, StringComparison.Ordinal);
        // When an identity could not be saved, keep the one this process already announced
        // instead of minting another on every check.
        var resolution = ConnectorConfigFiles.Resolve(
            user,
            saved,
            legacy,
            () => current is not null ? current[ConnectorConfigFiles.IdentityTokenKey] : newIdentity(),
            hostServerName);
        var values = resolution.Values;

        // Emptying a token that was set in the user file while the server runs means "disconnect",
        // not "fall back to the saved copy": clear the saved copy too. At startup an empty user
        // token is the fresh file an upgrade brings, and the saved token applies.
        if (!firstLoad && current is not null && userToken is not null
            && user is not null && user.TryGetValue(ConnectorConfigFiles.RegistrationTokenKey, out var emptied)
            && string.IsNullOrWhiteSpace(emptied))
        {
            var cleared = values.ToDictionary(entry => entry.Key, entry => entry.Value, StringComparer.Ordinal);
            cleared[ConnectorConfigFiles.RegistrationTokenKey] = "";
            values = cleared;
        }

        if (!ConnectorConfig.TryFromDictionary(values, out var config, out error) || config is null)
        {
            if (firstLoad)
            {
                return null;
            }

            Problem($"{error}; keeping the current settings");
            return null;
        }

        var written = true;
        var identity = values[ConnectorConfigFiles.IdentityTokenKey];
        if (userText is null && !userUnreadable)
        {
            written &= TryWrite(UserPath, () => ConnectorConfigFiles.SetValues(
                ConnectorConfigFiles.ShippedUserFile(),
                new Dictionary<string, string> { [ConnectorConfigFiles.IdentityTokenKey] = identity }));
        }
        else if (userText is not null && !userUnreadable && resolution.IdentitySource != IdentitySource.UserFile)
        {
            written &= TryWrite(UserPath, () => ConnectorConfigFiles.SetValues(
                userText,
                new Dictionary<string, string> { [ConnectorConfigFiles.IdentityTokenKey] = identity }));
        }

        if (!savedUnreadable)
        {
            var updates = ConnectorConfigFiles.SavedFileUpdates(values, saved);
            if (savedText is null)
            {
                written &= TryWrite(SavedPath, () => ConnectorConfigFiles.NewSavedFile(values));
            }
            else if (updates.Count > 0)
            {
                written &= TryWrite(SavedPath, () => ConnectorConfigFiles.SetValues(savedText, updates));
            }
        }

        if (resolution.IdentitySource == IdentitySource.Generated && current is null)
        {
            info($"Takaro Valheim generated a new identity for this server and saved it in {UserPath} and {SavedPath}.");
        }
        else if (resolution.IdentitySource == IdentitySource.LegacyServerName && current is null)
        {
            info($"Takaro Valheim keeps the identity this server already has in Takaro (its serverName, as releases up to 4.1 used) and saved it in {UserPath} and {SavedPath}.");
        }

        var previous = current;
        current = values;
        // After a failed write keep the previous one, so a cleared token is cleared again next check.
        userToken = !written ? userToken : user is not null && user.TryGetValue(ConnectorConfigFiles.RegistrationTokenKey, out var fromUser)
            && !string.IsNullOrWhiteSpace(fromUser)
                ? fromUser
                : null;
        // Our own writes must not count as a change; after a failed write no stamp is kept, so
        // the next check tries again.
        userStamp = written && !userUnreadable ? TryRead(UserPath) : null;
        savedStamp = written && !savedUnreadable ? TryRead(SavedPath) : null;
        lastProblem = null;

        var connectionChanged = previous is not null
            && ConnectorConfigFiles.ConnectionKeys.Any(key => previous[key] != values[key]);
        var restartKeys = previous is null
            ? Array.Empty<string>()
            : values.Keys
                .Where(key => !ConnectorConfigFiles.ConnectionKeys.Contains(key) && previous[key] != values[key])
                .ToArray();

        if (previous is null || connectionChanged)
        {
            info($"Takaro Valheim config loaded from {UserPath}. URL: {values[ConnectorConfigFiles.TakaroWsUrlKey]}, registration token: {TokenSource(user, saved)}.");
        }

        if (restartKeys.Length > 0)
        {
            warn($"Takaro Valheim: {string.Join(", ", restartKeys)} changed in {UserPath}; restart the server to apply (token, identity, URL and serverName apply at once).");
        }

        return new ConfigChange(config, connectionChanged, restartKeys);
    }

    private string TokenSource(IReadOnlyDictionary<string, string>? user, IReadOnlyDictionary<string, string>? saved)
    {
        if (Has(user, ConnectorConfigFiles.RegistrationTokenKey))
        {
            return "set";
        }

        return Has(saved, ConnectorConfigFiles.RegistrationTokenKey) ? $"set (kept in {SavedPath})" : "not set";
    }

    private static bool Has(IReadOnlyDictionary<string, string>? values, string key) =>
        values is not null && values.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value);

    private Dictionary<string, string>? ParseOrNull(string? text, string path, bool firstLoad, out bool unreadable)
    {
        unreadable = false;
        if (text is null)
        {
            return null;
        }

        try
        {
            return ConnectorConfigFiles.Parse(text);
        }
        catch (FormatException ex)
        {
            unreadable = true;
            Problem(firstLoad
                ? $"could not read {path} ({ex.Message}); starting without its settings and leaving the file as it is"
                : $"could not read {path} ({ex.Message}); keeping the current settings and trying again in a few seconds");
            return null;
        }
    }

    private bool TryWrite(string path, Func<string> render)
    {
        try
        {
            ConnectorConfigFiles.WriteAtomically(path, render());
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Problem($"could not write {path}: {ex.Message}. The connector keeps its settings in memory and retries");
            return false;
        }
    }

    // A failing read or write is retried every check; say so once, not every 5 s.
    private void Problem(string message)
    {
        if (message == lastProblem)
        {
            return;
        }

        lastProblem = message;
        warn($"Takaro Valheim {message}.");
    }

    // The whole text: a timestamp can miss an edit of the same length within the
    // filesystem's time resolution. False when the file exists but cannot be read now.
    private static bool ReadText(string path, out string? text)
    {
        try
        {
            text = File.Exists(path) ? File.ReadAllText(path) : null;
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            text = null;
            return false;
        }
    }

    private static string? TryRead(string path) => ReadText(path, out var text) ? text : null;
}
