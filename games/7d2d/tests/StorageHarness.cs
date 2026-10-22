using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using LiteDB;
using Takaro.Persistence;

// The connector's database against the real LiteDB 5.0.21 under Mono: the cause of
// "ReadFull must read PAGE_SIZE bytes", the write-through fix, every collection's boot
// lifecycle, and durability across clean restarts and kills during writes.
public static class StorageHarness
{
    private static int _assertions;

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 2 && args[0] == "--write-until-killed")
                return WriteUntilKilled(args[1]);

            string root = Path.Combine(Path.GetTempPath(), "takaro-storage-" + Guid.NewGuid());
            Directory.CreateDirectory(root);
            try
            {
                AssertPlainFileModeFailsUnderMono(Path.Combine(root, "plain"));
                AssertWriteThroughCheckpoints(Path.Combine(root, "write-through"));
                AssertBootLifecycle(Path.Combine(root, "lifecycle"));
                AssertRestartsAndKills(Path.Combine(root, "kills"));
            }
            finally
            {
                Directory.Delete(root, true);
            }
            Console.WriteLine("Storage harness passed: " + _assertions + " assertions");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    private static LiteDatabase Open(string directory, out Stream data, out Stream log)
    {
        Directory.CreateDirectory(directory);
        data = new WriteThroughFileStream(Path.Combine(directory, StorageLifecycle.FileName));
        log = new WriteThroughFileStream(Path.Combine(directory, StorageLifecycle.LogFileName));
        return new LiteDatabase(data, null, log);
    }

    private static void Close(LiteDatabase db, Stream data, Stream log)
    {
        db.Dispose();
        data.Dispose();
        log.Dispose();
    }

    private static PlayerRecord Player(string gameId, int i) =>
        new PlayerRecord
        {
            GameId = gameId,
            Name = "player-" + gameId,
            EntityId = 100 + i,
            Online = true,
            X = i,
            Y = 60,
            Z = -i,
            LastSeenUtc = DateTime.UtcNow,
        };

    // The reason the connector does not open LiteDB by filename: Mono's FileStream keeps a
    // write of exactly its buffer size (LiteDB's PAGE_SIZE) in memory, and the checkpoint
    // reads the log through another stream that sees the file one page short.
    private static void AssertPlainFileModeFailsUnderMono(string directory)
    {
        if (Type.GetType("Mono.Runtime") == null)
            return;
        Directory.CreateDirectory(directory);
        string error = null;
        try
        {
            using (
                var db = new LiteDatabase(
                    "Filename=" + Path.Combine(directory, "plain.db") + ";Connection=direct"
                )
            )
            {
                var players = db.GetCollection<PlayerRecord>("players");
                for (int i = 0; i < 20; i++)
                    players.Upsert(Player("p" + i, i));
                db.Checkpoint();
            }
        }
        catch (LiteException ex)
        {
            error = ex.Message;
        }
        True(
            error != null && error.StartsWith("ReadFull must read PAGE_SIZE bytes"),
            "LiteDB opened by filename fails its checkpoint under Mono (got: "
                + (error ?? "no error")
                + ")"
        );
    }

    private static void AssertWriteThroughCheckpoints(string directory)
    {
        LiteDatabase db = Open(directory, out Stream data, out Stream log);
        var players = db.GetCollection<PlayerRecord>("players");
        for (int i = 0; i < 2000; i++)
            players.Upsert(Player("p" + (i % 40), i));
        db.Checkpoint();
        Equal(40, players.Count(), "the write-through database checkpoints");
        Close(db, data, log);

        db = Open(directory, out data, out log);
        players = db.GetCollection<PlayerRecord>("players");
        Equal(40, players.Count(), "and reopens with every record");
        Equal(1999f, players.FindById("p39").X, "with the last written value");
        Close(db, data, log);
    }

    private static void AssertBootLifecycle(string directory)
    {
        True(
            StorageLifecycle
                .Collections.Keys.OrderBy(k => k)
                .SequenceEqual(
                    new[]
                    {
                        "bans",
                        "entities",
                        "inventories",
                        "items",
                        "locations",
                        "players",
                        "settings",
                        "waypoints",
                    }
                ),
            "every collection has a declared lifecycle"
        );
        foreach (
            string rebuilt in new[] { "items", "entities", "locations", "bans", "inventories" }
        )
            Equal(
                CollectionLifecycle.RebuiltAtBoot,
                StorageLifecycle.Collections[rebuilt],
                rebuilt + " is rebuilt at boot"
            );
        foreach (string durable in new[] { "players", "waypoints", "settings" })
            Equal(
                CollectionLifecycle.Durable,
                StorageLifecycle.Collections[durable],
                durable + " is durable"
            );

        LiteDatabase db = Open(directory, out Stream data, out Stream log);
        StorageLifecycle.ApplyAtBoot(db);
        db.GetCollection<PlayerRecord>("players").Insert(Player("online", 1));
        PlayerRecord offline = Player("offline", 2);
        offline.Online = false;
        db.GetCollection<PlayerRecord>("players").Insert(offline);
        db.GetCollection<ItemRecord>("items")
            .Insert(new ItemRecord { Code = "gunPistol", Name = "Pistol" });
        db.GetCollection<EntityRecord>("entities")
            .Insert(new EntityRecord { Code = "zombieBoe", Name = "Boe" });
        db.GetCollection<LocationRecord>("locations")
            .Insert(new LocationRecord { Code = "poi@1,2,3:r0" });
        db.GetCollection<BanRecord>("bans").Insert(new BanRecord { Id = "EOS_x", GameId = "x" });
        db.GetCollection<InventoryRecord>("inventories")
            .Insert(new InventoryRecord { GameId = "online", Items = new List<ItemSlot>() });
        db.GetCollection<BsonDocument>("waypoints")
            .Insert(new BsonDocument { ["_id"] = "home", ["owner"] = "online" });
        db.GetCollection<BsonDocument>("future").Insert(new BsonDocument { ["_id"] = "kept" });
        Close(db, data, log);

        db = Open(directory, out data, out log);
        StorageLifecycle.BootReport report = StorageLifecycle.ApplyAtBoot(db);
        foreach (
            string rebuilt in new[] { "items", "entities", "locations", "bans", "inventories" }
        )
        {
            Equal(
                0,
                db.GetCollection(rebuilt).Count(),
                rebuilt + " holds nothing stale after a restart"
            );
            True(report.Rebuilt.Contains(rebuilt), rebuilt + " is reported as rebuilt");
        }
        var players = db.GetCollection<PlayerRecord>("players");
        Equal(2, players.Count(), "players survive a restart");
        Equal(2, report.PlayersKept, "the report counts the registry");
        Equal(1, report.PlayersMarkedOffline, "the player online at shutdown is marked offline");
        True(players.FindAll().All(p => !p.Online), "nobody is online while the server boots");
        Equal(1f, players.FindById("online").X, "a player's last position survives");
        Equal(1, db.GetCollection("waypoints").Count(), "waypoints survive a restart");
        Equal(
            StorageLifecycle.SchemaVersion.ToString(),
            db.GetCollection<SettingRecord>("settings")
                .FindById(StorageLifecycle.SchemaVersionKey)
                .Value,
            "the schema version is recorded"
        );
        True(
            report.Unknown.SequenceEqual(new[] { "future" }),
            "a collection without a lifecycle is reported"
        );
        Equal(1, db.GetCollection("future").Count(), "and left untouched");
        Close(db, data, log);
    }

    // A child process updates positions in a loop until this process kills it (SIGKILL: no
    // dispose, no checkpoint) at a random point, mid-write; every reopen must succeed and
    // hold every commit the child had reported.
    private static void AssertRestartsAndKills(string directory)
    {
        var random = new Random(304);
        LiteDatabase db = Open(directory, out Stream data, out Stream log);
        StorageLifecycle.ApplyAtBoot(db);
        Close(db, data, log);

        for (int round = 0; round < 6; round++)
        {
            int killAfter = 50 + random.Next(1500);
            string exe = typeof(StorageHarness).Assembly.Location;
            var child = Process.Start(
                new ProcessStartInfo(
                    "mono",
                    "\"" + exe + "\" --write-until-killed \"" + directory + "\""
                )
                {
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                }
            );
            int lastReported = -1;
            for (int seen = 0; seen < killAfter; seen++)
            {
                string line = child.StandardOutput.ReadLine();
                if (line == null)
                    break;
                lastReported = int.Parse(line);
            }
            child.Kill();
            child.WaitForExit();
            True(
                lastReported >= 0,
                "round " + round + ": the writer committed before it was killed"
            );

            db = Open(directory, out data, out log);
            StorageLifecycle.ApplyAtBoot(db);
            var players = db.GetCollection<PlayerRecord>("players");
            Equal(10, players.Count(), "round " + round + ": the database opens with every player");
            True(
                players.FindAll().Max(p => p.X) >= lastReported,
                "round " + round + ": every commit reported before the kill survived it"
            );
            Close(db, data, log);
        }

        for (int restart = 0; restart < 10; restart++)
        {
            db = Open(directory, out data, out log);
            StorageLifecycle.ApplyAtBoot(db);
            Equal(
                10,
                db.GetCollection<PlayerRecord>("players").Count(),
                "clean restart " + restart + " keeps the registry"
            );
            Close(db, data, log);
        }
    }

    private static int WriteUntilKilled(string directory)
    {
        LiteDatabase db = Open(directory, out Stream data, out Stream log);
        var players = db.GetCollection<PlayerRecord>("players");
        for (int i = 0; i < 10; i++)
        {
            if (players.FindById("k" + i) == null)
                players.Insert(Player("k" + i, 0));
        }
        for (int i = (int)players.FindAll().Max(p => p.X) + 1; ; i++)
        {
            PlayerRecord record = players.FindById("k" + (i % 10));
            record.X = i;
            players.Update(record);
            Console.WriteLine(i);
            Console.Out.Flush();
        }
    }

    private static void True(bool condition, string description)
    {
        _assertions++;
        if (!condition)
            throw new Exception("Assertion failed: " + description);
    }

    private static void Equal<T>(T expected, T actual, string description)
    {
        _assertions++;
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new Exception(
                "Assertion failed: " + description + "; expected " + expected + ", got " + actual
            );
    }
}
