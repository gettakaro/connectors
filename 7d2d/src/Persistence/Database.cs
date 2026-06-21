using LiteDB;
using Takaro.Interfaces;
using Takaro.Services;

namespace Takaro.Persistence
{
    /// <summary>
    /// Owns the single LiteDatabase instance backing the state mirror.
    /// The database is in-memory: the mirror is rebuilt from game truth on every
    /// boot (seeding + events), so persistence has no value — and LiteDB 5's
    /// disk engine (WAL/checkpoint) has known failures under Mono
    /// ("ReadFull must read PAGE_SIZE bytes") that the memory backend avoids
    /// entirely.
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

        public LiteDatabase Db { get; private set; }

        // All DB access — DbWriter ops and WebSocket-thread reads — must hold
        // this lock; LiteDB's engine is not reliably safe under concurrent use.
        // Both sides run off the game thread, so contention never blocks the
        // game.
        public readonly object SyncRoot = new object();

        public ILiteCollection<PlayerRecord> Players => Db.GetCollection<PlayerRecord>("players");
        public ILiteCollection<InventoryRecord> Inventories =>
            Db.GetCollection<InventoryRecord>("inventories");
        public ILiteCollection<BanRecord> Bans => Db.GetCollection<BanRecord>("bans");
        public ILiteCollection<ItemRecord> Items => Db.GetCollection<ItemRecord>("items");

        public void OnInit()
        {
            Db = new LiteDatabase(":memory:");
            Players.EnsureIndex(p => p.Online);
            LogService.Instance.Info("In-memory state mirror database opened");
        }

        public void OnDestroy()
        {
            Db?.Dispose();
            Db = null;
        }
    }
}
