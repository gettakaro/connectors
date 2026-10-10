using System;
using System.IO;
using System.Security;
using System.Xml;

namespace Takaro.Config
{
    /// <summary>
    /// The settings one Config.xml holds. A null or empty field means the file
    /// does not set it.
    /// </summary>
    public sealed class ConfigValues
    {
        public string Url;
        public string RegistrationToken;
        public string IdentityToken;
        public bool? Enabled;
        public int? ReconnectIntervalSeconds;

        public bool SameConnection(ConfigValues other)
        {
            return other != null
                && Url == other.Url
                && RegistrationToken == other.RegistrationToken
                && IdentityToken == other.IdentityToken
                && Enabled == other.Enabled;
        }

        public bool SameAs(ConfigValues other)
        {
            return SameConnection(other)
                && ReconnectIntervalSeconds == other.ReconnectIntervalSeconds;
        }
    }

    /// <summary>
    /// Reading, merging and writing the two config files, with no game types so
    /// the contract harness can exercise it.
    ///
    /// The mod config (Mods/Takaro/Config.xml) ships in the release zip and is
    /// the file people edit. The saved config (&lt;server cwd&gt;/Takaro/Config.xml)
    /// is where releases up to 0.3 kept everything; the mod now writes the
    /// settings in use there, so replacing the mod folder on an upgrade keeps
    /// the token and the server's identity in Takaro.
    /// </summary>
    public static class ConfigFiles
    {
        public const string DefaultUrl = "wss://connect.takaro.io/";
        public const int DefaultReconnectIntervalSeconds = 30;

        // What releases before 0.2 wrote into a fresh config.
        public const string PlaceholderIdentityToken = "your-identity-token";

        public const string ModConfigHeader =
            "\n"
            + "  Takaro connection settings.\n"
            + "\n"
            + "  Paste the registration token Takaro shows when you add a Generic game server\n"
            + "  into RegistrationToken and save. This works before or after the server starts:\n"
            + "  the mod picks the change up within a few seconds, no restart needed.\n"
            + "\n"
            + "  Leave IdentityToken empty; the mod fills it in. It identifies this server in\n"
            + "  Takaro, so keep it if you move or reinstall the server.\n";

        public const string SavedConfigHeader =
            "\n"
            + "  Written by the Takaro mod: the settings it uses, kept outside Mods/ so that\n"
            + "  upgrading the mod keeps them. Edit Mods/Takaro/Config.xml instead; a\n"
            + "  RegistrationToken set there takes precedence over this file.\n";

        /// <summary>
        /// Reads one config file. Returns null when it does not exist; throws
        /// XmlException when it is not valid XML (for example, half-saved).
        /// </summary>
        public static ConfigValues Read(string path)
        {
            if (!File.Exists(path))
                return null;

            var doc = new XmlDocument();
            doc.Load(path);
            var values = new ConfigValues();
            XmlNode webSocket = doc.DocumentElement?.SelectSingleNode("WebSocket");
            if (webSocket == null)
                return values;

            values.Url = Text(webSocket, "Url");
            values.RegistrationToken = Text(webSocket, "RegistrationToken");
            values.IdentityToken = Text(webSocket, "IdentityToken");
            if (bool.TryParse(Text(webSocket, "Enabled"), out bool enabled))
                values.Enabled = enabled;
            if (
                int.TryParse(Text(webSocket, "ReconnectIntervalSeconds"), out int interval)
                && interval > 0
            )
                values.ReconnectIntervalSeconds = interval;
            return values;
        }

        /// <summary>
        /// The settings to use. The tokens come from the mod config when it sets
        /// them, else from the saved config. The other settings come from the
        /// mod config only when it changes them from the default: the shipped
        /// file spells the defaults out, and an upgrade must not let them
        /// override what the saved config holds. An identity that neither file
        /// holds comes from newIdentity, and identityGenerated says so.
        /// </summary>
        public static ConfigValues Resolve(
            ConfigValues mod,
            ConfigValues saved,
            Func<string> newIdentity,
            out bool identityGenerated
        )
        {
            identityGenerated = false;
            var resolved = new ConfigValues
            {
                Url =
                    (Set(mod?.Url) == DefaultUrl ? null : Set(mod?.Url))
                    ?? Set(saved?.Url)
                    ?? DefaultUrl,
                RegistrationToken =
                    Set(mod?.RegistrationToken) ?? Set(saved?.RegistrationToken) ?? "",
                IdentityToken = Identity(mod?.IdentityToken) ?? Identity(saved?.IdentityToken),
                Enabled = (mod?.Enabled == false ? false : (bool?)null) ?? saved?.Enabled ?? true,
                ReconnectIntervalSeconds =
                    (
                        mod?.ReconnectIntervalSeconds == DefaultReconnectIntervalSeconds
                            ? null
                            : mod?.ReconnectIntervalSeconds
                    )
                    ?? saved?.ReconnectIntervalSeconds
                    ?? DefaultReconnectIntervalSeconds,
            };
            if (resolved.IdentityToken == null)
            {
                resolved.IdentityToken = newIdentity();
                identityGenerated = true;
            }
            return resolved;
        }

        /// <summary>
        /// What the saved config should hold: the tokens in use, and its own
        /// other settings (the defaults for a new file). Settings made in the
        /// mod config are not copied, so setting one back to the default there
        /// takes effect instead of leaving the old value behind in this file.
        /// </summary>
        public static ConfigValues SavedCopy(ConfigValues resolved, ConfigValues saved)
        {
            return new ConfigValues
            {
                Url = Set(saved?.Url) ?? DefaultUrl,
                RegistrationToken = resolved.RegistrationToken,
                IdentityToken = resolved.IdentityToken,
                Enabled = saved?.Enabled ?? true,
                ReconnectIntervalSeconds =
                    saved?.ReconnectIntervalSeconds ?? DefaultReconnectIntervalSeconds,
            };
        }

        public static string Render(ConfigValues values, string header)
        {
            return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                + "<!--"
                + header
                + "-->\n"
                + "<Takaro>\n"
                + "  <WebSocket>\n"
                + Element("Url", values.Url)
                + Element("RegistrationToken", values.RegistrationToken)
                + Element("IdentityToken", values.IdentityToken)
                + Element("Enabled", (values.Enabled ?? true) ? "true" : "false")
                + Element(
                    "ReconnectIntervalSeconds",
                    (values.ReconnectIntervalSeconds ?? DefaultReconnectIntervalSeconds).ToString()
                )
                + "  </WebSocket>\n"
                + "</Takaro>\n";
        }

        /// <summary>
        /// Sets IdentityToken in an existing file, keeping its layout and
        /// comments, adding the element when it is missing.
        /// </summary>
        public static void WriteIdentity(string path, string identityToken)
        {
            var doc = new XmlDocument { PreserveWhitespace = true };
            doc.Load(path);
            XmlElement root = doc.DocumentElement;
            if (root == null)
                throw new XmlException("no root element");
            XmlNode webSocket = root.SelectSingleNode("WebSocket");
            if (webSocket == null)
                webSocket = root.AppendChild(doc.CreateElement("WebSocket"));
            XmlNode node = webSocket.SelectSingleNode("IdentityToken");
            if (node == null)
                node = webSocket.AppendChild(doc.CreateElement("IdentityToken"));
            node.InnerText = identityToken;
            WriteAtomically(path, doc.Save);
        }

        public static void WriteAll(string path, ConfigValues values, string header)
        {
            string text = Render(values, header);
            WriteAtomically(path, tmp => File.WriteAllText(tmp, text));
        }

        // A reader polling the file must never see it half-written.
        private static void WriteAtomically(string path, Action<string> write)
        {
            string tmp = path + ".tmp";
            write(tmp);
            if (File.Exists(path))
                File.Replace(tmp, path, null);
            else
                File.Move(tmp, path);
        }

        private static string Element(string name, string value)
        {
            return "    <" + name + ">" + SecurityElement.Escape(value ?? "") + "</" + name + ">\n";
        }

        private static string Text(XmlNode parent, string name)
        {
            return parent.SelectSingleNode(name)?.InnerText.Trim();
        }

        private static string Set(string value)
        {
            return string.IsNullOrEmpty(value) ? null : value;
        }

        private static string Identity(string value)
        {
            return value == PlaceholderIdentityToken ? null : Set(value);
        }
    }
}
