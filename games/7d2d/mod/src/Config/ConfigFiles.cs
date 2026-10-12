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
        public const int DefaultReconnectIntervalSeconds = 30;

        public const string Header =
            "\n"
            + "  Takaro connection settings.\n"
            + "\n"
            + "  Paste the registration token Takaro shows when you add a Generic game server\n"
            + "  into RegistrationToken and save. The mod picks the change up within a few\n"
            + "  seconds, no restart needed.\n"
            + "\n"
            + "  IdentityToken identifies this server in Takaro; the mod fills it in when it\n"
            + "  is empty. Keep it if you move or reinstall the server.\n";

        /// <summary>
        /// Reads the config, creating it when it is missing and writing a new
        /// identity into it when it has none; nothing else is ever written. A
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
            if (File.Exists(path))
            {
                values = Read(path);
                if (values.IdentityToken != "")
                    return values;
                values.IdentityToken = newIdentity();
            }
            else
            {
                values = new ConfigValues { IdentityToken = newIdentity() };
            }

            try
            {
                if (File.Exists(path))
                    WriteIdentity(path, values.IdentityToken);
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
            var doc = new XmlDocument();
            doc.Load(path);
            var values = new ConfigValues();
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
                + "<Takaro>\n"
                + "  <WebSocket>\n"
                + Element("Url", values.Url)
                + Element("RegistrationToken", values.RegistrationToken)
                + Element("IdentityToken", values.IdentityToken)
                + Element("Enabled", values.Enabled ? "true" : "false")
                + Element("ReconnectIntervalSeconds", values.ReconnectIntervalSeconds.ToString())
                + "  </WebSocket>\n"
                + "</Takaro>\n";
        }

        // Sets IdentityToken, keeping the file's layout and comments.
        private static void WriteIdentity(string path, string identityToken)
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
