using System;
using System.Collections.Generic;
using System.Linq;
using LiteDB;

namespace Takaro.Persistence
{
    public enum CollectionLifecycle
    {
        /// <summary>Emptied when the database opens and refilled from game truth.</summary>
        RebuiltAtBoot,

        /// <summary>Kept across restarts; only the connector's own writes change it.</summary>
        Durable,
    }

    /// <summary>
    /// What happens to each collection of the connector's database when the server boots.
    /// Every collection the mod uses is listed here; a new collection is added to this table
    /// with its lifecycle, and to the table in DEVELOPMENT.md.
    /// </summary>
    public static class StorageLifecycle
    {
        public const int SchemaVersion = 1;
        public const string FileName = "takaro.db";
        public const string LogFileName = "takaro-log.db";

        public const string Players = "players";
        public const string Inventories = "inventories";
        public const string Bans = "bans";
        public const string Items = "items";
        public const string Entities = "entities";
        public const string Locations = "locations";
        public const string Waypoints = "waypoints";
        public const string Settings = "settings";

        public const string SchemaVersionKey = "schemaVersion";

        public static readonly IReadOnlyDictionary<string, CollectionLifecycle> Collections =
            new Dictionary<string, CollectionLifecycle>
            {
                { Items, CollectionLifecycle.RebuiltAtBoot },
                { Entities, CollectionLifecycle.RebuiltAtBoot },
                { Locations, CollectionLifecycle.RebuiltAtBoot },
                { Bans, CollectionLifecycle.RebuiltAtBoot },
                { Inventories, CollectionLifecycle.RebuiltAtBoot },
                { Players, CollectionLifecycle.Durable },
                { Waypoints, CollectionLifecycle.Durable },
                { Settings, CollectionLifecycle.Durable },
            };

        public sealed class BootReport
        {
            public List<string> Rebuilt = new List<string>();
            public int PlayersKept;
            public int PlayersMarkedOffline;
            public List<string> Unknown = new List<string>();
        }

        /// <summary>
        /// Applies every collection's lifecycle to a freshly opened database: rebuilt
        /// collections are dropped, every registered player is marked offline (nobody is
        /// connected while the server boots, also after a crash), and the schema version is
        /// recorded. Collections this table does not know are reported, never touched.
        /// </summary>
        public static BootReport ApplyAtBoot(LiteDatabase db)
        {
            var report = new BootReport();
            foreach (string name in db.GetCollectionNames().ToList())
            {
                if (!Collections.TryGetValue(name, out CollectionLifecycle lifecycle))
                {
                    report.Unknown.Add(name);
                    continue;
                }
                if (lifecycle == CollectionLifecycle.RebuiltAtBoot)
                {
                    db.DropCollection(name);
                    report.Rebuilt.Add(name);
                }
            }

            ILiteCollection<PlayerRecord> players = db.GetCollection<PlayerRecord>(Players);
            foreach (PlayerRecord record in players.Find(p => p.Online).ToList())
            {
                record.Online = false;
                players.Update(record);
                report.PlayersMarkedOffline++;
            }
            report.PlayersKept = players.Count();
            players.EnsureIndex(p => p.Online);

            db.GetCollection<SettingRecord>(Settings)
                .Upsert(
                    new SettingRecord
                    {
                        Key = SchemaVersionKey,
                        Value = SchemaVersion.ToString(),
                        UpdatedUtc = DateTime.UtcNow,
                    }
                );
            db.Checkpoint();
            return report;
        }
    }
}
