using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using System.Xml;

namespace Takaro.Config
{
    /// <summary>The settings Config.xml holds, with defaults for what it leaves out.</summary>
    public sealed class ConfigValues
    {
        public string Url = ConfigFiles.DefaultUrl;
        public string RegistrationToken = "";
        public string IdentityToken = "";
        public bool Enabled = true;
        public int ReconnectIntervalSeconds = ConfigFiles.DefaultReconnectIntervalSeconds;
        public string ServerChatName = "";
        public bool ReportCharacters = true;

        /// <summary>
        /// Set by Load when it brought a file from an older layout up to
        /// ConfigFiles.Version. Not part of the file.
        /// </summary>
        public bool Upgraded;

        public bool SameConnection(ConfigValues other)
        {
            return other != null
                && Url == other.Url
                && RegistrationToken == other.RegistrationToken
                && IdentityToken == other.IdentityToken
                && Enabled == other.Enabled;
        }
    }

    /// <summary>
    /// Reading and writing &lt;server cwd&gt;/Takaro/Config.xml, with no game
    /// types so the contract harness can exercise it. The release zip does not
    /// contain this file, so replacing Mods/Takaro on an upgrade never touches it.
    /// </summary>
    public static class ConfigFiles
    {
        public const string DefaultUrl = "wss://connect.takaro.io/";

        // The base of the backoff after consecutive failures; a drop after a
        // stable connection is retried within about a second whatever it says.
        public const int DefaultReconnectIntervalSeconds = 2;

        /// <summary>
        /// The layout version on the root element. Version 2 turned
        /// ReportCharacters on by default. A file without it was written by an
        /// older release, which always wrote ReportCharacters as false (its
        /// default); that false is read as "not chosen" and the file is
        /// upgraded once. In a version 2 file false is the admin's choice.
        /// </summary>
        public const int Version = 2;

        // The line older releases wrote; Update swaps it for the current one,
        // which is the end of Header.
        private const string OldCharactersHint =
            "  Turn it on only when your Takaro supports Characters, then restart the server.\n";
        public const string CharactersHint =
            "  It is on by default; set it to false to keep lives out of Takaro (for example\n"
            + "  when your Takaro is older than 2.3), then restart the server.\n";

        public const string Header =
            "\n"
            + "  Takaro connection settings.\n"
            + "\n"
            + "  Paste the registration token Takaro shows when you add a Generic game server\n"
            + "  into RegistrationToken and save. The mod picks the change up within a few\n"
            + "  seconds, no restart needed.\n"
            + "\n"
            + "  IdentityToken identifies this server in Takaro; the mod fills it in when it\n"
            + "  is empty. Keep it if you move or reinstall the server.\n"
            + "\n"
            + "  ServerChatName is the sender players see on messages from Takaro, instead\n"
            + "  of \"Server\". The chat name set in Takaro wins over it; leave both empty\n"
            + "  for \"Server\".\n"
            + "\n"
            + "  ReportCharacters tells Takaro about each life on a Permanent Death server.\n"
            + "  It is on by default; set it to false to keep lives out of Takaro (for example\n"
            + "  when your Takaro is older than 2.3), then restart the server.\n";

        /// <summary>
        /// Reads the config, creating it when it is missing, writing a new
        /// identity into it when it has none and bringing a file from an older
        /// layout up to Version; nothing else is ever written. A
        /// failed write is returned in writeError, and the values (with the new
        /// identity) are still returned. Throws XmlException when the file is
        /// not valid XML (for example, half-saved) and IOException when it
        /// cannot be read.
        /// </summary>
        public static ConfigValues Load(
            string path,
            Func<string> newIdentity,
            out bool wrote,
            out Exception writeError
        )
        {
            wrote = false;
            writeError = null;
            ConfigValues values;
            bool needsUpgrade = false;
            if (File.Exists(path))
            {
                values = Read(path, out int version);
                needsUpgrade = version < Version;
                if (values.IdentityToken != "" && !needsUpgrade)
                    return values;
                if (values.IdentityToken == "")
                    values.IdentityToken = newIdentity();
            }
            else
            {
                values = new ConfigValues { IdentityToken = newIdentity() };
            }

            try
            {
                if (File.Exists(path))
                {
                    Update(path, values);
                    values.Upgraded = needsUpgrade;
                }
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    string text = Render(values);
                    WriteAtomically(path, tmp => File.WriteAllText(tmp, text));
                }
                wrote = true;
            }
            catch (Exception ex)
                when (ex is IOException || ex is UnauthorizedAccessException || ex is XmlException)
            {
                writeError = ex;
            }
            return values;
        }

        public static ConfigValues Read(string path)
        {
            return Read(path, out _);
        }

        public static ConfigValues Read(string path, out int version)
        {
            var doc = new XmlDocument();
            doc.Load(path);
            var values = new ConfigValues();
            version = FileVersion(doc);
            XmlNode chat = doc.DocumentElement?.SelectSingleNode("Chat");
            if (chat != null)
                values.ServerChatName = Text(chat, "ServerChatName");
            XmlNode players = doc.DocumentElement?.SelectSingleNode("Players");
            if (
                players != null
                && bool.TryParse(Text(players, "ReportCharacters"), out bool report)
                // An older release wrote its default false into every file.
                && (report || version >= Version)
            )
                values.ReportCharacters = report;
            XmlNode webSocket = doc.DocumentElement?.SelectSingleNode("WebSocket");
            if (webSocket == null)
                return values;

            string url = Text(webSocket, "Url");
            if (url != "")
                values.Url = url;
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

        public static string Render(ConfigValues values)
        {
            return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                + "<!--"
                + Header
                + "-->\n"
                + "<Takaro version=\""
                + Version
                + "\">\n"
                + "  <WebSocket>\n"
                + Element("Url", values.Url)
                + Element("RegistrationToken", values.RegistrationToken)
                + Element("IdentityToken", values.IdentityToken)
                + Element("Enabled", values.Enabled ? "true" : "false")
                + Element("ReconnectIntervalSeconds", values.ReconnectIntervalSeconds.ToString())
                + "  </WebSocket>\n"
                + "  <Chat>\n"
                + Element("ServerChatName", values.ServerChatName)
                + "  </Chat>\n"
                + "  <Players>\n"
                + Element("ReportCharacters", values.ReportCharacters ? "true" : "false")
                + "  </Players>\n"
                + "</Takaro>\n";
        }

        private static int FileVersion(XmlDocument doc)
        {
            string text = doc.DocumentElement?.GetAttribute("version") ?? "";
            return int.TryParse(text, out int version) && version > 0 ? version : 1;
        }

        // Sets IdentityToken and, for a file from an older layout, the version
        // and the ReportCharacters value Read gave it, keeping the file's
        // layout, comments and every other setting.
        private static void Update(string path, ConfigValues values)
        {
            var doc = new XmlDocument { PreserveWhitespace = true };
            doc.Load(path);
            XmlElement root = doc.DocumentElement;
            if (root == null)
                throw new XmlException("no root element");
            SetChild(doc, root, "WebSocket", "IdentityToken", values.IdentityToken);
            if (FileVersion(doc) < Version)
            {
                root.SetAttribute("version", Version.ToString());
                SetChild(
                    doc,
                    root,
                    "Players",
                    "ReportCharacters",
                    values.ReportCharacters ? "true" : "false"
                );
                foreach (XmlNode node in doc.ChildNodes)
                {
                    if (node is XmlComment comment && comment.Value.Contains(OldCharactersHint))
                        comment.Value = comment.Value.Replace(OldCharactersHint, CharactersHint);
                }
            }
            WriteAtomically(path, doc.Save);
        }

        private static void SetChild(
            XmlDocument doc,
            XmlElement root,
            string section,
            string name,
            string value
        )
        {
            XmlNode parent = root.SelectSingleNode(section);
            if (parent == null)
                parent = root.AppendChild(doc.CreateElement(section));
            XmlNode node = parent.SelectSingleNode(name);
            if (node == null)
                node = parent.AppendChild(doc.CreateElement(name));
            node.InnerText = value;
        }

        // A reader polling the file must never see it half-written. The
        // temporary file starts as a copy so it carries the file's permissions
        // (it holds the registration token), and writing into it keeps them.
        private static void WriteAtomically(string path, Action<string> write)
        {
            string tmp = path + ".tmp";
            if (File.Exists(tmp))
                File.Delete(tmp);
            if (File.Exists(path))
            {
                File.Copy(path, tmp);
                write(tmp);
                File.Replace(tmp, path, null);
            }
            else
            {
                write(tmp);
                OwnerOnly(tmp);
                File.Move(tmp, path);
            }
        }

        [DllImport("libc.so.6", EntryPoint = "chmod", SetLastError = true)]
        private static extern int Chmod(string path, uint mode);

        // The registration token goes into this file: on Linux a new one is
        // readable by its owner only (0600), not by every local user.
        private static void OwnerOnly(string path)
        {
            if (Environment.OSVersion.Platform != PlatformID.Unix)
                return;
            try
            {
                Chmod(path, Convert.ToUInt32("600", 8));
            }
            catch (Exception ex)
                when (ex is DllNotFoundException || ex is EntryPointNotFoundException) { }
        }

        private static string Element(string name, string value)
        {
            return "    <" + name + ">" + SecurityElement.Escape(value ?? "") + "</" + name + ">\n";
        }

        private static string Text(XmlNode parent, string name)
        {
            return parent.SelectSingleNode(name)?.InnerText.Trim() ?? "";
        }
    }
}
