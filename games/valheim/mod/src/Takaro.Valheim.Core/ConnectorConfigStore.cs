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

    /// <summary>The URL, the token, the identity or serverName changed: the connection must be made afresh.</summary>
    public bool ConnectionChanged { get; }

    /// <summary>Changed settings that only a server restart applies.</summary>
    public IReadOnlyList<string> RestartKeys { get; }
}

/// <summary>
/// Loads the connector settings from BepInEx/config/com.takaro.valheim.cfg, creates that file
/// when it is missing, writes a new identity into it when it has none, and tells a caller
/// polling it when the file changed. Nothing else is ever written. Thread-safe: the plugin calls
/// <see cref="Load"/> once on the main thread and <see cref="CheckForChanges"/> from a timer.
/// </summary>
public sealed class ConnectorConfigStore
{
    private readonly object gate = new();
    private readonly TimeSpan settleDelay;
    private readonly Action<string> info;
    private readonly Action<string> warn;
    private readonly Func<string> newIdentity;
    private readonly string? hostServerName;
    private IReadOnlyDictionary<string, string>? current;
    private string? stamp;
    private string? lastProblem;

    public ConnectorConfigStore(
        string path,
        Action<string>? info = null,
        Action<string>? warn = null,
        Func<string>? newIdentity = null,
        TimeSpan? settleDelay = null,
        string? hostServerName = null)
    {
        FilePath = path;
        this.info = info ?? (_ => { });
        this.warn = warn ?? (_ => { });
        this.newIdentity = newIdentity ?? (() => Guid.NewGuid().ToString());
        this.settleDelay = settleDelay ?? TimeSpan.FromSeconds(1);
        this.hostServerName = hostServerName;
    }

    /// <summary>The config file: BepInEx/config/com.takaro.valheim.cfg.</summary>
    public string FilePath { get; }

    /// <summary>
    /// The first load. Returns false, with the reason, only when the settings are invalid (for
    /// example an over-long chatSenderName). A file that cannot be read counts as empty and is
    /// left as it is.
    /// </summary>
    public bool Load(out ConnectorConfig? config, out string error)
    {
        lock (gate)
        {
            config = Reload(firstLoad: true, out error)?.Config;
            return config is not null;
        }
    }

    /// <summary>
    /// Re-reads the file when its text changed. Returns null when nothing changed or the file
    /// could not be read or parsed (the current settings stay, and the next check retries).
    /// </summary>
    public ConfigChange? CheckForChanges()
    {
        lock (gate)
        {
            if (current is null || !ReadText(FilePath, out var text) || text == stamp)
            {
                return null;
            }

            // An editor saving in place can be caught half-way with a truncated but readable
            // token; only apply a text that stays the same for a moment.
            Thread.Sleep(settleDelay);
            if (!ReadText(FilePath, out var again) || again != text)
            {
                return null;
            }

            return Reload(firstLoad: false, readable: true, text, out _);
        }
    }

    private ConfigChange? Reload(bool firstLoad, out string error)
    {
        var readable = ReadText(FilePath, out var text);
        return Reload(firstLoad, readable, text, out error);
    }

    // text is the content already read (and, while running, confirmed by the settle re-read).
    private ConfigChange? Reload(bool firstLoad, bool readable, string? text, out string error)
    {
        error = "";
        Dictionary<string, string>? file = null;
        if (text is null)
        {
            if (!firstLoad)
            {
                Problem(readable
                    ? $"config {FilePath} is missing; keeping the current settings"
                    : $"could not read {FilePath}; keeping the current settings and trying again in a few seconds");
                return null;
            }
        }
        else
        {
            try
            {
                file = ConnectorConfigFiles.Parse(text);
            }
            catch (FormatException ex)
            {
                Problem(firstLoad
                    ? $"could not read {FilePath} ({ex.Message}); starting without its settings and leaving the file as it is"
                    : $"could not read {FilePath} ({ex.Message}); keeping the current settings and trying again in a few seconds");
                if (!firstLoad)
                {
                    return null;
                }
            }
        }

        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in ConnectorConfigFiles.Defaults)
        {
            values[entry.Key] = file is not null && file.TryGetValue(entry.Key, out var value) && !string.IsNullOrWhiteSpace(value)
                ? value.Trim()
                : entry.Value;
        }

        // An identity that could not be saved stays the one this process already announced.
        var identity = values[ConnectorConfigFiles.IdentityTokenKey];
        var generated = identity.Length == 0;
        if (generated)
        {
            identity = current?[ConnectorConfigFiles.IdentityTokenKey] ?? newIdentity();
            values[ConnectorConfigFiles.IdentityTokenKey] = identity;
        }

        if (values[ConnectorConfigFiles.ServerNameKey].Length == 0)
        {
            values[ConnectorConfigFiles.ServerNameKey] = ConnectorConfigFiles.UniqueServerName(identity, hostServerName);
        }

        if (!ConnectorConfig.TryFromDictionary(values, out var config, out error) || config is null)
        {
            if (!firstLoad)
            {
                Problem($"{error}; keeping the current settings");
            }

            return null;
        }

        var written = true;
        if (readable && text is null)
        {
            written = TryWrite(() => ConnectorConfigFiles.NewFile(identity, values[ConnectorConfigFiles.ServerNameKey]));
            if (written)
            {
                info($"Takaro Valheim created {FilePath} with a new identity for this server.");
            }
        }
        else if (file is not null && generated)
        {
            // Only over the text this was built from: an edit saved meanwhile is kept and the
            // identity is written on the next check instead.
            written = ReadText(FilePath, out var unchanged) && unchanged == text
                && TryWrite(() => ConnectorConfigFiles.SetValues(
                    text!,
                    new Dictionary<string, string> { [ConnectorConfigFiles.IdentityTokenKey] = identity }));
            if (written)
            {
                info($"Takaro Valheim generated a new identity for this server and saved it in {FilePath}.");
            }
        }

        var previous = current;
        current = values;
        // The connector's own write is not a change; after a failed write no stamp is kept, so
        // the next check tries again.
        stamp = !written ? null : file is null && text is not null ? text : TryRead(FilePath);
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
            var token = values[ConnectorConfigFiles.RegistrationTokenKey].Length > 0 ? "set" : "not set";
            info($"Takaro Valheim config loaded from {FilePath}. URL: {values[ConnectorConfigFiles.TakaroWsUrlKey]}, registration token: {token}.");
        }

        if (restartKeys.Length > 0)
        {
            warn($"Takaro Valheim: {string.Join(", ", restartKeys)} changed in {FilePath}; restart the server to apply (token, identity, URL and serverName apply at once).");
        }

        return new ConfigChange(config, connectionChanged, restartKeys);
    }

    private bool TryWrite(Func<string> render)
    {
        try
        {
            ConnectorConfigFiles.WriteAtomically(FilePath, render());
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Problem($"could not write {FilePath}: {ex.Message}. The connector keeps its identity in memory and retries");
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
