using System;
using System.IO;
using LiteDB;

namespace Takaro.Persistence
{
    /// <summary>
    /// The database file and its write-ahead log, opened on <see cref="WriteThroughFileStream"/>s
    /// with every collection's boot lifecycle applied. Game-free, so the storage harness opens
    /// it exactly as the mod does.
    /// </summary>
    public sealed class DatabaseFiles : IDisposable
    {
        private readonly WriteThroughFileStream _data;
        private readonly WriteThroughFileStream _log;

        public LiteDatabase Db { get; }
        public string DataPath { get; }
        public StorageLifecycle.BootReport Report { get; }

        private DatabaseFiles(string dataPath, string logPath)
        {
            DataPath = dataPath;
            _data = new WriteThroughFileStream(dataPath);
            _log = new WriteThroughFileStream(logPath);
            try
            {
                Db = new LiteDatabase(_data, null, _log);
                Report = StorageLifecycle.ApplyAtBoot(Db);
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        /// <summary>
        /// Opens the database in <paramref name="directory"/>. When LiteDB cannot read it, the
        /// log is set aside first, because the data file alone holds everything up to the last
        /// checkpoint; only when the data file is unreadable too is it set aside as well and a
        /// new database started. Every step is reported through <paramref name="error"/>. An
        /// IOException (the file is held by another process) is the caller's to handle.
        /// </summary>
        public static DatabaseFiles Open(string directory, Action<string> error)
        {
            Directory.CreateDirectory(directory);
            string dataPath = Path.Combine(directory, StorageLifecycle.FileName);
            string logPath = Path.Combine(directory, StorageLifecycle.LogFileName);
            string suffix = ".unreadable-" + DateTime.UtcNow.ToString("yyyyMMddTHHmmssZ");
            try
            {
                return new DatabaseFiles(dataPath, logPath);
            }
            catch (LiteException ex) when (File.Exists(logPath) && new FileInfo(logPath).Length > 0)
            {
                error(
                    $"Database {dataPath} could not be read ({ex.Message}); set its log aside as {StorageLifecycle.LogFileName}{suffix}, changes after the last checkpoint are lost"
                );
                File.Move(logPath, logPath + suffix);
            }
            catch (LiteException ex)
            {
                return StartOver(dataPath, logPath, suffix, ex, error);
            }

            try
            {
                return new DatabaseFiles(dataPath, logPath);
            }
            catch (LiteException ex)
            {
                return StartOver(dataPath, logPath, suffix, ex, error);
            }
        }

        private static DatabaseFiles StartOver(
            string dataPath,
            string logPath,
            string suffix,
            LiteException ex,
            Action<string> error
        )
        {
            error(
                $"Database {dataPath} could not be read ({ex.Message}); set it aside as {StorageLifecycle.FileName}{suffix} and started a new one"
            );
            File.Move(dataPath, dataPath + suffix);
            if (File.Exists(logPath))
            {
                // After the first step the log is the empty one the retry created.
                if (File.Exists(logPath + suffix))
                    File.Delete(logPath);
                else
                    File.Move(logPath, logPath + suffix);
            }
            return new DatabaseFiles(dataPath, logPath);
        }

        public void Dispose()
        {
            // Disposing LiteDB checkpoints the log into the data file; the streams are ours.
            Db?.Dispose();
            _data?.Dispose();
            _log?.Dispose();
        }
    }
}
