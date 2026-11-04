using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace TakaroEmpyrion.Core
{
    /// <summary>
    /// TakaroConfig.txt next to the mod DLL, key=value, '#' comments. Every key is documented in
    /// TakaroConfig.txt.example. Unknown keys are ignored.
    /// </summary>
    public sealed class Config
    {
        public string WsUrl = "wss://connect.takaro.io/";
        public string IdentityToken;
        public string RegistrationToken;
        public string ServerName = "Empyrion";
        public string ChatSenderName;
        public bool Debug;
        public bool EnableDestructiveCommands;
        public HashSet<string> ConsoleAllowlist = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "help", "plys", "list", "say", "kick", "ban", "unban", "mute", "servers", "instances", "time", "gents", "difficulties",
        };
        public int TelnetPort;
        public string TelnetPassword;
        /// <summary>1 (default): offer protocol v1 with the Capability Manifest and the planet maps. 0: v0 only.</summary>
        public int ProtocolVersion = 1;

        public static Config Load(string path)
        {
            var c = new Config();
            if (!File.Exists(path)) throw new FileNotFoundException("TakaroConfig.txt not found", path);
            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim().ToUpperInvariant();
                string v = line.Substring(eq + 1).Trim();
                switch (k)
                {
                    case "TAKARO_WS_URL": if (v.Length > 0) c.WsUrl = v; break;
                    case "IDENTITY_TOKEN": c.IdentityToken = v; break;
                    case "REGISTRATION_TOKEN": c.RegistrationToken = v; break;
                    case "SERVER_NAME": if (v.Length > 0) c.ServerName = v; break;
                    case "CHAT_SENDER_NAME": c.ChatSenderName = v; break;
                    case "DEBUG": c.Debug = Bool(v); break;
                    case "ENABLE_DESTRUCTIVE_COMMANDS": c.EnableDestructiveCommands = Bool(v); break;
                    case "CONSOLE_ALLOWLIST":
                        c.ConsoleAllowlist = new HashSet<string>(
                            v.Split(new[] { ',', ' ' }, StringSplitOptions.RemoveEmptyEntries), StringComparer.OrdinalIgnoreCase);
                        break;
                    case "TELNET_PORT": int.TryParse(v, out c.TelnetPort); break;
                    case "TELNET_PASSWORD": c.TelnetPassword = v; break;
                    case "PROTOCOL_VERSION": if (int.TryParse(v, out int pv)) c.ProtocolVersion = pv <= 0 ? 0 : 1; break;
                }
            }
            if (string.IsNullOrEmpty(c.IdentityToken))
                c.IdentityToken = "empyrion-" + Regex.Replace(c.ServerName.ToLowerInvariant(), "[^a-z0-9]+", "-").Trim('-');
            return c;
        }

        static bool Bool(string v) { return v == "1" || v.Equals("true", StringComparison.OrdinalIgnoreCase) || v.Equals("yes", StringComparison.OrdinalIgnoreCase); }

        /// <summary>
        /// Fills TelnetPort/TelnetPassword from the dedi's own config (-dedicated &lt;yaml&gt;) when the operator
        /// did not set them, so console output works without extra setup.
        /// </summary>
        public void ReadTelnetFromDedicatedYaml(string rootDir, string[] commandLine)
        {
            if (TelnetPort > 0 && !string.IsNullOrEmpty(TelnetPassword)) return;
            string yaml = "dedicated.yaml";
            for (int i = 0; i + 1 < commandLine.Length; i++)
                if (commandLine[i].Equals("-dedicated", StringComparison.OrdinalIgnoreCase)) yaml = commandLine[i + 1];
            string path = Path.IsPathRooted(yaml) ? yaml : Path.Combine(rootDir, yaml);
            if (!File.Exists(path)) return;
            string text = File.ReadAllText(path);
            if (!YamlBool(text, "Tel_Enabled")) return;
            if (TelnetPort <= 0) TelnetPort = int.TryParse(YamlValue(text, "Tel_Port"), out int p) ? p : 30004;
            if (string.IsNullOrEmpty(TelnetPassword)) TelnetPassword = YamlValue(text, "Tel_Pwd");
        }

        static string YamlValue(string text, string key)
        {
            Match m = Regex.Match(text, "^[ \\t]*" + key + "[ \\t]*:[ \\t]*(\"[^\"]*\"|'[^']*'|[^#\\r\\n]*)", RegexOptions.Multiline);
            if (!m.Success) return null;
            return m.Groups[1].Value.Trim().Trim('"', '\'');
        }

        static bool YamlBool(string text, string key)
        {
            string v = YamlValue(text, key);
            return v != null && v.Equals("true", StringComparison.OrdinalIgnoreCase);
        }
    }
}
