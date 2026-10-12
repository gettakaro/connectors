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

        // Often enough that a pasted token feels immediate; the file is tiny.
        private const int WATCH_INTERVAL_MILLISECONDS = 5000;

        // An editor may save in several writes; a change counts once the text
        // has stayed the same for this long.
        private const int SETTLE_MILLISECONDS = 1000;

        public static readonly string ConfigPath = Path.GetFullPath(
            Path.Combine(API.BasePath, "Config.xml")
        );

        public string WebSocketUrl { get; private set; } = ConfigFiles.DefaultUrl;
        public string IdentityToken { get; private set; } = "";
        public string RegistrationToken { get; private set; } = "";
        public bool WebSocketEnabled { get; private set; } = true;
        public int ReconnectIntervalSeconds { get; private set; } =
            ConfigFiles.DefaultReconnectIntervalSeconds;

        /// <summary>
        /// Raised on the watcher thread when a change to Config.xml alters the
        /// URL, a token or Enabled.
        /// </summary>
        public event Action ConnectionSettingsChanged;

        private readonly object _loadLock = new object();
        private ConfigValues _current;
        private string _stamp;
        private Timer _watchTimer;

        // Set while a generated identity is not yet in the file, so the retry
        // writes the identity this process already announced.
        private bool _identityUnsaved;
        private string _lastWriteError;
        private string _lastReadError;

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
                string text = Stamp(ConfigPath);
                if (text == _stamp)
                    return;
                Thread.Sleep(SETTLE_MILLISECONDS);
                if (Stamp(ConfigPath) != text)
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
        /// Reads the file and applies it. Returns true when the connection
        /// settings changed. An unreadable file leaves the current settings
        /// untouched and is retried on the next check.
        /// </summary>
        private bool Reload()
        {
            lock (_loadLock)
            {
                bool existed = File.Exists(ConfigPath);
                ConfigValues values;
                bool wrote;
                Exception writeError;
                try
                {
                    values = ConfigFiles.Load(
                        ConfigPath,
                        () => _identityUnsaved ? _current.IdentityToken : Guid.NewGuid().ToString(),
                        out wrote,
                        out writeError
                    );
                }
                catch (Exception ex)
                    when (ex is XmlException
                        || ex is IOException
                        || ex is UnauthorizedAccessException
                    )
                {
                    // Retried every check; say so once per problem, not every 5 s.
                    string error = $"Could not read {ConfigPath} ({ex.Message})";
                    if (error != _lastReadError)
                        LogService.Instance.Warn(
                            error
                                + "; keeping the current settings and trying again in a few seconds"
                        );
                    _lastReadError = error;
                    _stamp = null;
                    return false;
                }

                _lastReadError = null;
                if (wrote)
                    LogService.Instance.Info(
                        existed
                            ? $"Wrote a new identity token into {ConfigPath}"
                            : $"Created {ConfigPath} with a new identity token"
                    );
                _identityUnsaved = writeError != null;
                if (writeError != null)
                {
                    // Retried every check; say so once, not every 5 s.
                    string error = $"Could not write {ConfigPath}: {writeError.Message}";
                    if (error != _lastWriteError)
                        LogService.Instance.Error(
                            error + ". The mod uses the new identity in memory and retries."
                        );
                    _lastWriteError = error;
                }

                bool firstLoad = _current == null;
                bool connectionChanged = !firstLoad && !values.SameConnection(_current);
                _current = values;
                WebSocketUrl = values.Url;
                RegistrationToken = values.RegistrationToken;
                IdentityToken = values.IdentityToken;
                WebSocketEnabled = values.Enabled;
                ReconnectIntervalSeconds = values.ReconnectIntervalSeconds;

                // Our own write must not count as a change; after a failed
                // write no stamp is kept, so the next check tries again.
                _stamp = writeError != null ? null : Stamp(ConfigPath);

                if (firstLoad || connectionChanged)
                    LogService.Instance.Info(
                        $"Config loaded from {ConfigPath}. WebSocket URL: {WebSocketUrl}, "
                            + $"Enabled: {WebSocketEnabled}, registration token: "
                            + (RegistrationToken == "" ? "not set" : "set")
                    );
                return connectionChanged;
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
