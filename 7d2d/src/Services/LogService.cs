using System;
using System.IO;
using Takaro;
using Takaro.Interfaces;
namespace Takaro.Services
{
    public class LogService : IService
    {
        
        private static readonly string LogFolderPath = Path.Combine(API.BasePath, "logs");
        
        private static volatile LogService _instance;
        private static readonly object Lock = new object();

        public static LogService Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (Lock)
                {
                    if (_instance == null)
                        _instance = new LogService();
                }
                return _instance;
            }
        }


        public bool LogDebug = false;
        public void OnInit()
        {
            if (!Directory.Exists(LogFolderPath))
                Directory.CreateDirectory(LogFolderPath);
            Instance.Info("LogService initialized");
        }

        public void Debug(string message, string dir = "")
        {
            if (!LogDebug)
                return;
            Log.Out($"[{API.ModPrefix}] *DEBUG* {(string.IsNullOrEmpty(dir) ? "" : dir + ": ")}{message}");
            Write(DateTime.Now.ToString("MM/dd/yyyy HH:mm:ss") + " *DEBUG* " + message, dir);
        }

        public void Info(string message, string dir = "")
        {
            Log.Out($"[{API.ModPrefix}] *INFO* {(string.IsNullOrEmpty(dir) ? "" : dir + ": ")}{message}");
            Write(DateTime.Now.ToString("MM/dd/yyyy HH:mm:ss") + " *INFO* " + message, dir);
        }

        public void Warn(string message, string dir = "")
        {
            Log.Out($"[{API.ModPrefix}] *WARN* {(string.IsNullOrEmpty(dir) ? "" : dir + ": ")}{message}");
            Write(DateTime.Now.ToString("MM/dd/yyyy HH:mm:ss") + " *WARN* " + message, dir);
        }
        
        public void Error(string message, string dir = "")
        {
            message = CleanInvalidChars(message);
            dir = CleanInvalidChars(dir);
    
            Log.Out($"[{API.ModPrefix}] *ERROR* {(string.IsNullOrEmpty(dir) ? "" : dir + ": ")}{message}");
            Write(DateTime.Now.ToString("MM/dd/yyyy HH:mm:ss") + " *ERROR* " + message, dir);
        }

        private string CleanInvalidChars(string input)
        {
            foreach (char invalidChar in Path.GetInvalidPathChars())
            {
                input = input.Replace(invalidChar.ToString(), "");
            }

            return input;
        }

        private void Write(string message, string dir = "")
        {
            dir = CleanInvalidChars(dir);  // clean the directory name

            var filename = $"{DateTime.Today:M-d-yyyy}.log";
            string path;
            if (string.IsNullOrEmpty(dir))
                path = Path.Combine(LogFolderPath, filename);
            else
            {
                if (!Directory.Exists(Path.Combine(API.BasePath, dir)))
                    Directory.CreateDirectory(dir);
                path = Path.Combine(LogFolderPath, dir, filename);
            }
            using (var fs = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
            using (var sw = new StreamWriter(fs))
            {
                sw.WriteLine(message);
            }
        }

        public void OnDestroy() {}
    }
}