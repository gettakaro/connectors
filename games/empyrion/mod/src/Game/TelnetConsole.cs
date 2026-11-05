using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace TakaroEmpyrion.Game
{
    /// <summary>
    /// Runs console commands through the dedi's own telnet console on 127.0.0.1 (same host, same process
    /// tree) so executeConsoleCommand can return the game's real output. The legacy Request_ConsoleCommand
    /// path executes but returns no text. One persistent session, serialised, reconnects on failure.
    /// </summary>
    public sealed class TelnetConsole
    {
        readonly int _port;
        readonly string _password;
        readonly object _lock = new object();
        TcpClient _tcp;
        NetworkStream _stream;

        public TelnetConsole(int port, string password) { _port = port; _password = password; }

        public bool Configured { get { return _port > 0 && !string.IsNullOrEmpty(_password); } }

        public string Run(string command, TimeSpan quiet, TimeSpan max)
        {
            lock (_lock)
            {
                for (int attempt = 0; attempt < 2; attempt++)
                {
                    try
                    {
                        EnsureConnected();
                        Drain(TimeSpan.FromMilliseconds(50), TimeSpan.FromMilliseconds(200));
                        byte[] b = Encoding.UTF8.GetBytes(command.Replace("\r", " ").Replace("\n", " ") + "\r\n");
                        _stream.Write(b, 0, b.Length);
                        return Clean(Drain(quiet, max));
                    }
                    catch (Exception) when (attempt == 0) { Close(); }
                }
                throw new IOException("telnet console unavailable");
            }
        }

        void EnsureConnected()
        {
            if (_tcp != null && _tcp.Connected) return;
            Close();
            _tcp = new TcpClient();
            _tcp.Connect("127.0.0.1", _port);
            _stream = _tcp.GetStream();
            string banner = Drain(TimeSpan.FromMilliseconds(300), TimeSpan.FromSeconds(3));
            if (banner.IndexOf("password", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                byte[] pw = Encoding.UTF8.GetBytes(_password + "\r\n");
                _stream.Write(pw, 0, pw.Length);
                string reply = Drain(TimeSpan.FromMilliseconds(400), TimeSpan.FromSeconds(3));
                if (reply.IndexOf("success", StringComparison.OrdinalIgnoreCase) < 0 && reply.IndexOf("Logged in", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    Close();
                    throw new IOException("telnet login refused");
                }
            }
        }

        string Drain(TimeSpan quiet, TimeSpan max)
        {
            var sb = new StringBuilder();
            var buf = new byte[8192];
            DateTime end = DateTime.UtcNow + max;
            DateTime lastData = DateTime.UtcNow;
            while (DateTime.UtcNow < end)
            {
                if (_stream.DataAvailable)
                {
                    int n = _stream.Read(buf, 0, buf.Length);
                    if (n <= 0) throw new IOException("telnet closed");
                    sb.Append(Encoding.UTF8.GetString(buf, 0, n));
                    lastData = DateTime.UtcNow;
                    continue;
                }
                if (sb.Length > 0 && DateTime.UtcNow - lastData >= quiet) break;
                if (sb.Length == 0 && DateTime.UtcNow - lastData >= max) break;
                Thread.Sleep(20);
            }
            return sb.ToString();
        }

        static string Clean(string s)
        {
            var sb = new StringBuilder();
            foreach (char c in s) if (c >= ' ' || c == '\n' || c == '\t') sb.Append(c);
            return sb.ToString().Trim();
        }

        public void Close()
        {
            try { _stream?.Dispose(); } catch (Exception) { }
            try { _tcp?.Close(); } catch (Exception) { }
            _stream = null; _tcp = null;
        }
    }
}
