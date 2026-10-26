using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Takaro.WebSocket
{
    /// <summary>
    /// What the mod tells Takaro about itself and the game (Generic Game
    /// Protocol v1, Game Profile): the connector identity sent in identify, the
    /// game.profile capability and the bytes getGameIcon serves. Pure data; the
    /// game build, mod version and icon bytes are handed in by API.InitMod.
    /// </summary>
    public static class GameProfile
    {
        public const string ConnectorName = "takaro-7d2d";
        public const string DisplayName = "7 Days to Die";
        public const string Capability = "game.profile";
        public const string IconResourceName = "Takaro.icon-256.png";

        public const int MaxGameVersionLength = 32;
        public const int MaxIconBytes = 128 * 1024;
        public const int IconSide = 256;
        public const int MaxConnectorVersionLength = 64;

        // Semantic Versioning 2.0.0, as Takaro validates connector.version.
        private static readonly Regex StrictSemver = new Regex(
            @"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                + @"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
                + @"(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$",
            RegexOptions.CultureInvariant
        );

        private static readonly Regex ModInfoVersion = new Regex(
            @"<Version\s+value\s*=\s*""([^""]*)""",
            RegexOptions.CultureInvariant
        );

        private static readonly byte[] PngSignature = { 137, 80, 78, 71, 13, 10, 26, 10 };

        /// <summary>Null when the mod's own version is not strict semver.</summary>
        public static string ConnectorVersion { get; private set; }

        /// <summary>Null when the game build is unknown.</summary>
        public static string GameVersion { get; private set; }

        /// <summary>Null when the shipped icon is missing or unusable.</summary>
        public static byte[] Icon { get; private set; }

        public static string IconRevision { get; private set; }

        public static void Configure(string connectorVersion, string gameVersion, byte[] icon)
        {
            string version = connectorVersion?.Trim();
            ConnectorVersion = IsStrictSemver(version) ? version : null;
            GameVersion = NormalizeGameVersion(gameVersion);
            Icon = IsServableIcon(icon) ? icon : null;
            IconRevision = Icon == null ? null : RevisionOf(Icon);
        }

        public static bool IsStrictSemver(string version)
        {
            return !string.IsNullOrEmpty(version)
                && version.Length <= MaxConnectorVersionLength
                && StrictSemver.IsMatch(version);
        }

        /// <summary>The value of ModInfo.xml's Version element, which release-please and
        /// the release build stamp.</summary>
        public static string ReadModInfoVersion(string modInfoXml)
        {
            if (string.IsNullOrEmpty(modInfoXml))
                return null;
            Match match = ModInfoVersion.Match(modInfoXml);
            return match.Success ? match.Groups[1].Value : null;
        }

        /// <summary>Trimmed, at most 32 printable ASCII characters, or null.</summary>
        public static string NormalizeGameVersion(string version)
        {
            string trimmed = version?.Trim();
            if (string.IsNullOrEmpty(trimmed))
                return null;
            var printable = new StringBuilder(trimmed.Length);
            foreach (char c in trimmed)
            {
                if (c >= 0x20 && c <= 0x7E)
                    printable.Append(c);
            }
            string result = printable.ToString().Trim();
            if (result.Length > MaxGameVersionLength)
                result = result.Substring(0, MaxGameVersionLength).TrimEnd();
            return result.Length == 0 ? null : result;
        }

        /// <summary>The first 12 hex characters of the PNG's sha256.</summary>
        public static string RevisionOf(byte[] icon)
        {
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(icon);
                var hex = new StringBuilder(12);
                for (int i = 0; i < 6; i++)
                    hex.Append(hash[i].ToString("x2"));
                return hex.ToString();
            }
        }

        /// <summary>A PNG whose IHDR says 256 x 256, within Takaro's size limit.</summary>
        public static bool IsServableIcon(byte[] icon)
        {
            if (icon == null || icon.Length < 24 || icon.Length > MaxIconBytes)
                return false;
            for (int i = 0; i < PngSignature.Length; i++)
            {
                if (icon[i] != PngSignature[i])
                    return false;
            }
            if (Encoding.ASCII.GetString(icon, 12, 4) != "IHDR")
                return false;
            return ReadBigEndian(icon, 16) == IconSide && ReadBigEndian(icon, 20) == IconSide;
        }

        public static Dictionary<string, object> BuildConnectorIdentity()
        {
            if (ConnectorVersion == null)
                return null;
            return new Dictionary<string, object>
            {
                { "name", ConnectorName },
                { "version", ConnectorVersion },
            };
        }

        public static Dictionary<string, object> BuildCapability()
        {
            var parameters = new Dictionary<string, object> { { "name", DisplayName } };
            if (GameVersion != null)
                parameters["version"] = GameVersion;
            if (Icon != null)
            {
                parameters["icon"] = new Dictionary<string, object>
                {
                    { "revision", IconRevision },
                    { "size", Icon.Length },
                };
            }
            return new Dictionary<string, object>
            {
                { "name", Capability },
                { "parameters", parameters },
            };
        }

        /// <summary>The getGameIcon answer: the declared revision and the whole file.</summary>
        public static Dictionary<string, object> BuildIconResponse()
        {
            if (Icon == null)
                return null;
            return new Dictionary<string, object>
            {
                { "revision", IconRevision },
                { "data", Convert.ToBase64String(Icon) },
            };
        }

        private static int ReadBigEndian(byte[] bytes, int offset)
        {
            return (bytes[offset] << 24)
                | (bytes[offset + 1] << 16)
                | (bytes[offset + 2] << 8)
                | bytes[offset + 3];
        }
    }
}
