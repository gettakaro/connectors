using System;
using System.IO;
using System.Threading;
using System.Xml;
using Takaro.Services;

namespace Takaro.Config
{
    public class ConfigManager
    {
        private static ConfigManager _instance;
        private static readonly object _lock = new object();

        // Often enough that a pasted token feels immediate; the files are tiny.
        private const int WATCH_INTERVAL_MILLISECONDS = 5000;

        public static string ModConfigPath { get; private set; } =
            Path.Combine(
                Path.Combine(Directory.GetCurrentDirectory(), "Mods/Takaro"),
                "Config.xml"
            );
        public static readonly string SavedConfigPath = Path.Combine(API.BasePath, "Config.xml");

        public string WebSocketUrl { get; private set; } = ConfigFiles.DefaultUrl;
        public string IdentityToken { get; private set; } = "";
        public string RegistrationToken { get; private set; } = "";
        public bool WebSocketEnabled { get; private set; } = true;
        public int ReconnectIntervalSeconds { get; private set; } =
            ConfigFiles.DefaultReconnectIntervalSeconds;

        /// <summary>
        /// Raised on the watcher thread when a config file change alters the URL,
        /// a token or Enabled.
        /// </summary>
        public event Action ConnectionSettingsChanged;

        private readonly object _loadLock = new object();
        private ConfigValues _current;
        private string _modStamp;
        private string _savedStamp;
        private Timer _watchTimer;

        private ConfigManager() { }

        public static ConfigManager Instance
        {
            get
            {
                if (_instance == null)
                {
                    lock (_lock)
                    {
                        if (_instance == null)
                        {
                            _instance = new ConfigManager();
                        }
                    }
                }
                return _instance;
            }
        }

        /// <summary>The folder the mod was loaded from; call before LoadConfig.</summary>
        public static void UseModFolder(string folder)
        {
            // Mod.Path reads like ".../7DaysToDieServer_Data/../Mods/Takaro";
            // people copy this path from the log, so give them the plain one.
            ModConfigPath = Path.GetFullPath(Path.Combine(folder, "Config.xml"));
        }

        public void LoadConfig()
        {
            Reload();
        }

        public void StartWatching()
        {
            lock (_loadLock)
            {
                if (_watchTimer != null)
                    return;
                _watchTimer = new Timer(
                    _ => CheckForChanges(),
                    null,
                    WATCH_INTERVAL_MILLISECONDS,
                    WATCH_INTERVAL_MILLISECONDS
                );
            }
        }

        public void StopWatching()
        {
            lock (_loadLock)
            {
                _watchTimer?.Dispose();
                _watchTimer = null;
            }
        }

        private void CheckForChanges()
        {
            try
            {
                if (Stamp(ModConfigPath) == _modStamp && Stamp(SavedConfigPath) == _savedStamp)
                    return;
                if (Reload())
                    ConnectionSettingsChanged?.Invoke();
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error checking Config.xml for changes: {ex.Message}");
            }
        }

        /// <summary>
        /// Reads both files and applies the result. Returns true when the
        /// connection settings changed. An unreadable file leaves the current
        /// settings untouched and is retried on the next check.
        /// </summary>
        private bool Reload()
        {
            lock (_loadLock)
            {
                string modStamp = Stamp(ModConfigPath);
                string savedStamp = Stamp(SavedConfigPath);
                ConfigValues mod;
                ConfigValues saved;
                try
                {
                    mod = ConfigFiles.Read(ModConfigPath);
                    saved = ConfigFiles.Read(SavedConfigPath);
                }
                catch (Exception ex) when (ex is XmlException || ex is IOException)
                {
                    LogService.Instance.Warn(
                        $"Could not read the Takaro config ({ex.Message}); "
                            + "keeping the current settings and trying again in a few seconds"
                    );
                    return false;
                }

                // When an identity could not be saved, keep using the one this
                // process already announced instead of minting another.
                ConfigValues resolved = ConfigFiles.Resolve(
                    mod,
                    saved,
                    () => _current?.IdentityToken ?? Guid.NewGuid().ToString(),
                    out bool identityGenerated
                );
                bool written = true;

                if (mod == null)
                    written &= TryWrite(
                        "create",
                        ModConfigPath,
                        () =>
                            ConfigFiles.WriteAll(
                                ModConfigPath,
                                new ConfigValues
                                {
                                    Url = resolved.Url,
                                    Enabled = resolved.Enabled,
                                    ReconnectIntervalSeconds = resolved.ReconnectIntervalSeconds,
                                    IdentityToken = identityGenerated ? resolved.IdentityToken : "",
                                },
                                ConfigFiles.ModConfigHeader
                            )
                    );
                else if (identityGenerated)
                    written &= TryWrite(
                        "update",
                        ModConfigPath,
                        () => ConfigFiles.WriteIdentity(ModConfigPath, resolved.IdentityToken)
                    );
                if (identityGenerated && _current == null)
                    LogService.Instance.Info("Generated a new identity token");

                ConfigValues savedCopy = ConfigFiles.SavedCopy(resolved, saved);
                bool savedChanged = !savedCopy.SameAs(saved);
                if (savedChanged)
                    written &= TryWrite(
                        "update",
                        SavedConfigPath,
                        () =>
                            ConfigFiles.WriteAll(
                                SavedConfigPath,
                                savedCopy,
                                ConfigFiles.SavedConfigHeader
                            )
                    );

                bool connectionChanged = _current != null && !resolved.SameConnection(_current);
                bool firstLoad = _current == null;
                _current = resolved;
                WebSocketUrl = resolved.Url;
                RegistrationToken = resolved.RegistrationToken;
                IdentityToken = resolved.IdentityToken;
                WebSocketEnabled = resolved.Enabled ?? true;
                ReconnectIntervalSeconds =
                    resolved.ReconnectIntervalSeconds
                    ?? ConfigFiles.DefaultReconnectIntervalSeconds;

                // Our own writes must not count as a change. After a failed
                // write, no stamp is kept, so the next check tries again.
                _modStamp =
                    !written ? null
                    : mod == null || identityGenerated ? Stamp(ModConfigPath)
                    : modStamp;
                _savedStamp =
                    !written ? null
                    : savedChanged ? Stamp(SavedConfigPath)
                    : savedStamp;

                if (firstLoad || connectionChanged)
                    LogService.Instance.Info(
                        $"Config loaded from {ModConfigPath}. WebSocket URL: {WebSocketUrl}, "
                            + $"Enabled: {WebSocketEnabled}, registration token: "
                            + TokenSource(mod, saved)
                    );
                return connectionChanged;
            }
        }

        private static string TokenSource(ConfigValues mod, ConfigValues saved)
        {
            if (!string.IsNullOrEmpty(mod?.RegistrationToken))
                return "set";
            if (!string.IsNullOrEmpty(saved?.RegistrationToken))
                return $"set (kept in {SavedConfigPath})";
            return "not set";
        }

        // A failing write is retried every check; say so once, not every 5 s.
        private string _lastWriteError;

        private bool TryWrite(string verb, string path, Action write)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                write();
                return true;
            }
            catch (Exception ex)
            {
                string error = $"Could not {verb} {path}: {ex.Message}";
                if (error != _lastWriteError)
                    LogService.Instance.Error(
                        error + ". The mod keeps its settings in memory and retries."
                    );
                _lastWriteError = error;
                return false;
            }
        }

        // The whole text: a timestamp can miss an edit of the same length
        // within the filesystem's time resolution.
        private static string Stamp(string path)
        {
            try
            {
                return File.Exists(path) ? File.ReadAllText(path) : "";
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
