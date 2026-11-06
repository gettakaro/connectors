using System;
using System.IO;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Core
{
    /// <summary>Writes to Logs/takaro.log in the mod folder (rotated at 10 MB) and mirrors warnings to the game log.</summary>
    public sealed class FileLog : ILog
    {
        readonly string _path;
        readonly object _lock = new object();
        readonly Action<string> _gameLog;
        public bool DebugEnabled;

        public FileLog(string dir, Action<string> gameLog)
        {
            Directory.CreateDirectory(dir);
            _path = Path.Combine(dir, "takaro.log");
            _gameLog = gameLog;
        }

        void Write(string level, string s)
        {
            string line = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ") + " " + level + " " + s;
            lock (_lock)
            {
                try
                {
                    var fi = new FileInfo(_path);
                    if (fi.Exists && fi.Length > 10 * 1024 * 1024)
                    {
                        string old = _path + ".1";
                        if (File.Exists(old)) File.Delete(old);
                        File.Move(_path, old);
                    }
                    File.AppendAllText(_path, line + Environment.NewLine);
                }
                catch (Exception) { }
            }
            if (level != "DEBUG" && level != "INFO" && _gameLog != null)
            {
                try { _gameLog("[Takaro] " + level + " " + s); } catch (Exception) { }
            }
        }

        public void Debug(string s) { if (DebugEnabled) Write("DEBUG", s); }
        public void Info(string s) { Write("INFO", s); }
        public void Warn(string s) { Write("WARN", s); }
        public void Error(string s) { Write("ERROR", s); }
    }
}
