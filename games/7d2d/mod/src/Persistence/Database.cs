using System;
using System.IO;
using LiteDB;
using Takaro.Interfaces;
using Takaro.Services;

namespace Takaro.Persistence
{
    /// <summary>
    /// Owns the single LiteDatabase instance: a file in the world's save directory
    /// (<see cref="StorageLifecycle.FileName"/>), so it belongs to that world and moves with
    /// its backups. The file is opened at GameStartDone, when the save directory is known,
    /// and before anything is written. What each collection does on boot is
    /// <see cref="StorageLifecycle"/>; how the files are opened is <see cref="DatabaseFiles"/>.
    /// </summary>
    public class Database : IService
    {
        private static Database _instance;
        private static readonly object _lock = new object();

        public static Database Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new Database();
                }
                return _instance;
            }
        }

        private DatabaseFiles _files;

        public LiteDatabase Db { get; private set; }

        public bool IsOpen => Db != null;

        /// <summary>The database file, or null while the database is in memory.</summary>
        public string FilePath { get; private set; }

        // All DB access — DbWriter ops and WebSocket-thread reads — must hold
        // this lock; LiteDB's engine is not reliably safe under concurrent use.
        // Both sides run off the game thread, so contention never blocks the
        // game.
        public readonly object SyncRoot = new object();

        public ILiteCollection<PlayerRecord> Players =>
            Db.GetCollection<PlayerRecord>(StorageLifecycle.Players);
        public ILiteCollection<InventoryRecord> Inventories =>
            Db.GetCollection<InventoryRecord>(StorageLifecycle.Inventories);
        public ILiteCollection<BanRecord> Bans =>
            Db.GetCollection<BanRecord>(StorageLifecycle.Bans);
        public ILiteCollection<ItemRecord> Items =>
            Db.GetCollection<ItemRecord>(StorageLifecycle.Items);
        public ILiteCollection<EntityRecord> Entities =>
            Db.GetCollection<EntityRecord>(StorageLifecycle.Entities);
        public ILiteCollection<LocationRecord> Locations =>
            Db.GetCollection<LocationRecord>(StorageLifecycle.Locations);
        public ILiteCollection<SettingRecord> Settings =>
            Db.GetCollection<SettingRecord>(StorageLifecycle.Settings);

        public void OnInit() { }

        /// <summary>
        /// Opens (or creates) the database in <paramref name="directory"/> and applies every
        /// collection's boot lifecycle (<see cref="DatabaseFiles.Open"/> recovers from a file
        /// LiteDB cannot read). A file that cannot be opened at all (another process holds it)
        /// leaves the connector on an in-memory database, so the server still works without a
        /// registry. Both are logged as errors.
        /// </summary>
        public void Open(string directory)
        {
            lock (SyncRoot)
            {
                if (Db != null)
                    return;

                StorageLifecycle.BootReport report;
                try
                {
                    _files = DatabaseFiles.Open(
                        directory,
                        message => LogService.Instance.Error(message)
                    );
                    Db = _files.Db;
                    FilePath = _files.DataPath;
                    report = _files.Report;
                }
                catch (IOException ex)
                {
                    LogService.Instance.Error(
                        $"Database in {directory} could not be opened ({ex.Message}); using an in-memory database, the player registry will not persist"
                    );
                    Db = new LiteDatabase(":memory:");
                    report = StorageLifecycle.ApplyAtBoot(Db);
                }

                LogService.Instance.Info(
                    $"Database opened ({FilePath ?? ":memory:"}): rebuilt [{string.Join(", ", report.Rebuilt)}], "
                        + $"{report.PlayersKept} registered players ({report.PlayersMarkedOffline} marked offline)"
                );
                if (report.Unknown.Count > 0)
                    LogService.Instance.Warn(
                        $"Database has collections without a lifecycle, left untouched: {string.Join(", ", report.Unknown)}"
                    );
            }
        }

        public void OnDestroy()
        {
            lock (SyncRoot)
            {
                if (_files != null)
                    _files.Dispose();
                else
                    Db?.Dispose();
                _files = null;
                Db = null;
                FilePath = null;
            }
        }
    }
}
