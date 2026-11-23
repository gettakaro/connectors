using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Protocol
{
    /// <summary>What the mod tells Takaro about itself and the game: identify's game and connector, the game.profile Capability and the getGameIcon answer.</summary>
    public sealed class GameProfile
    {
        public const string GameIdentifier = "empyrion", ConnectorName = "takaro-empyrion", DisplayName = "Empyrion - Galactic Survival",
            Capability = "game.profile", IconResource = "TakaroEmpyrion.icon-256.png";
        const int MaxGameVersionLength = 32, MaxIconBytes = 128 * 1024;
        static readonly Regex Semver = new Regex(@"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$", RegexOptions.CultureInvariant);

        readonly string _connectorVersion, _gameVersion, _iconRevision;
        readonly byte[] _icon;

        public GameProfile(string connectorVersion, string gameVersion, byte[] icon)
        {
            _connectorVersion = connectorVersion != null && Semver.IsMatch(connectorVersion) ? connectorVersion : null;
            _gameVersion = IsValidGameVersion(gameVersion) ? gameVersion : null;
            if (icon != null && icon.Length > 0 && icon.Length <= MaxIconBytes)
            {
                _icon = icon;
                using (var sha = SHA256.Create())
                    _iconRevision = string.Concat(sha.ComputeHash(icon).Take(6).Select(b => b.ToString("x2")));
            }
        }

        static bool IsValidGameVersion(string version)
        {
            return !string.IsNullOrEmpty(version) && version.Length <= MaxGameVersionLength && version.All(c => c >= 0x20 && c <= 0x7E);
        }

        /// <summary>BuildNumber.txt holds the build on line one and the version on line two; gives "1.19.6 b5188", or null when unreadable.</summary>
        public static string ParseBuildNumber(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return null;
            string[] lines = text.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0).ToArray();
            if (lines.Length < 2) return null;
            return lines[1].TrimStart('v') + " b" + lines[0];
        }

        /// <summary>The game version from the server root, which holds Content/Mods/&lt;mod&gt;; null when the file is missing.</summary>
        public static string ReadGameVersion(string modDir)
        {
            try { return ParseBuildNumber(File.ReadAllText(Path.Combine(modDir, "..", "..", "..", "BuildNumber.txt"), Encoding.UTF8)); }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        public static byte[] LoadEmbeddedIcon()
        {
            using (Stream stream = typeof(GameProfile).Assembly.GetManifestResourceStream(IconResource))
            {
                if (stream == null) return null;
                using (var copy = new MemoryStream())
                {
                    stream.CopyTo(copy);
                    return copy.ToArray();
                }
            }
        }

        public void AddToIdentify(JObject payload)
        {
            payload["game"] = GameIdentifier;
            // Takaro refuses the whole identify on a malformed connector version.
            if (_connectorVersion != null) payload["connector"] = new JObject { ["name"] = ConnectorName, ["version"] = _connectorVersion };
        }

        public JObject ToCapability()
        {
            var parameters = new JObject { ["name"] = DisplayName };
            if (_gameVersion != null) parameters["version"] = _gameVersion;
            if (_icon != null) parameters["icon"] = new JObject { ["revision"] = _iconRevision, ["size"] = _icon.Length };
            return new JObject { ["name"] = Capability, ["parameters"] = parameters };
        }

        /// <summary>The getGameIcon answer, or null without a usable icon.</summary>
        public JObject IconAnswer()
        {
            return _icon == null ? null : new JObject { ["revision"] = _iconRevision, ["data"] = Convert.ToBase64String(_icon) };
        }
    }
}
