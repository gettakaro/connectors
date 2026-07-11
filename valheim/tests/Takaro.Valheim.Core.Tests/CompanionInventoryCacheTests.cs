using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionInventoryCacheTests
{
    private const long PeerId = 7_654_321;
    private const string SessionNonce = "session-current";

    private static readonly DateTimeOffset Now =
        DateTimeOffset.Parse("2026-07-11T10:00:00+02:00");

    private static readonly TakaroPlayer Player =
        new("Steam_1", "Odin", "1", "steam:1", null, null);

    [TestMethod]
    public void InventoryAcceptsConfirmedEmptySnapshot()
    {
        var cache = CreateCache();

        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            Player,
            Array.Empty<CompanionInventoryStack>(),
            Now));

        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(Player.GameId, Now.AddSeconds(29), out var items));
        Assert.AreEqual(0, items.Count);
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet("  STEAM:1  ", Now.AddSeconds(29), out _));
    }

    [TestMethod]
    public void InventoryRejectsOversizedMalformedOrNegativeStacks()
    {
        var cache = CreateCache();
        var valid = Stack("Wood", "Wood", amount: 1, quality: 1, durability: 100, slot: 0);
        var invalidSnapshots = new IReadOnlyList<CompanionInventoryStack>[]
        {
            Enumerable.Repeat(valid, CompanionProtocol.MaximumInventoryStacks + 1).ToArray(),
            new CompanionInventoryStack[] { null! },
            new[] { valid with { Code = " " } },
            new[] { valid with { Code = new string('c', CompanionProtocol.MaximumCodeCharacters + 1) } },
            new[] { valid with { Name = " " } },
            new[] { valid with { Name = new string('n', CompanionProtocol.MaximumChatCharacters + 1) } },
            new[] { valid with { Amount = 0 } },
            new[] { valid with { Amount = -1 } },
            new[] { valid with { Amount = CompanionProtocol.MaximumInventoryAmount + 1 } },
            new[] { valid with { Quality = 0 } },
            new[] { valid with { Quality = -1 } },
            new[] { valid with { Quality = CompanionProtocol.MaximumItemQuality + 1 } },
            new[] { valid with { Durability = -1 } },
            new[] { valid with { Durability = float.NaN } },
            new[] { valid with { Durability = float.PositiveInfinity } },
            new[] { valid with { Durability = CompanionProtocol.MaximumDurability + 128f } },
            new[] { valid with { Slot = -1 } },
            new[] { valid with { Slot = CompanionProtocol.MaximumInventorySlot + 1 } }
        };

        Assert.IsFalse(cache.Remember(PeerId, SessionNonce, Player, null!, Now));
        foreach (var invalid in invalidSnapshots)
        {
            Assert.IsFalse(cache.Remember(PeerId, SessionNonce, Player, invalid, Now));
        }

        Assert.AreEqual(
            CompanionInventoryState.Missing,
            cache.TryGet(Player.GameId, Now, out _));
    }

    [TestMethod]
    public void OlderSnapshotCannotOverwriteNewerSnapshot()
    {
        var cache = CreateCache();
        var newerPlayer = Player with { Name = "Odin New" };
        var olderPlayer = Player with { Name = "Odin Old" };

        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            newerPlayer,
            new[] { Stack("Stone", "Stone", amount: 2, slot: 1) },
            Now));
        Assert.IsFalse(cache.Remember(
            PeerId,
            SessionNonce,
            olderPlayer,
            new[] { Stack("Wood", "Wood", amount: 99, slot: 0) },
            Now.AddSeconds(-1)));

        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(newerPlayer.Name, Now, out var items));
        Assert.AreEqual("Stone", items.Single().Code);
        Assert.AreEqual(2, items.Single().Amount);
        Assert.AreEqual(
            CompanionInventoryState.Missing,
            cache.TryGet(olderPlayer.Name, Now, out _));

        var renamedPlayer = newerPlayer with { Name = "Odin Renamed" };
        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            renamedPlayer,
            new[] { Stack("Wood", "Wood", slot: 0) },
            Now.AddSeconds(1)));
        Assert.AreEqual(
            CompanionInventoryState.Missing,
            cache.TryGet(newerPlayer.Name, Now.AddSeconds(1), out _),
            "Replacing a player snapshot must remove aliases from the old observation.");
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(renamedPlayer.Name, Now.AddSeconds(1), out _));
    }

    [TestMethod]
    public void ExpiredInventoryReturnsUnavailableInsteadOfFabricatedEmpty()
    {
        var cache = CreateCache();
        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            Player,
            new[] { Stack("Wood", "Wood", slot: 0) },
            Now));

        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(Player.GameId, Now.AddSeconds(29.999), out var fresh));
        Assert.AreEqual(1, fresh.Count);

        Assert.AreEqual(
            CompanionInventoryState.Expired,
            cache.TryGet(Player.GameId, Now.AddSeconds(30), out var expired),
            "The freshness boundary is exclusive: age equal to TTL is expired.");
        Assert.AreEqual(0, expired.Count);
        Assert.AreEqual(
            CompanionInventoryState.Expired,
            cache.TryGet(Player.GameId, Now.AddMinutes(1), out _));
    }

    [TestMethod]
    public void RemovePeerAndWorldResetClearInventoryAliases()
    {
        var cache = new CompanionInventoryCache();
        cache.SwitchWorld("world-a");
        cache.BeginSession(PeerId, SessionNonce);
        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            Player,
            new[] { Stack("Wood", "Wood", slot: 0) },
            Now));

        cache.RemovePeer(PeerId);
        AssertAllAliasesAreMissing(cache, Player);

        cache.BeginSession(PeerId, SessionNonce);
        Assert.IsTrue(cache.Remember(
            PeerId,
            SessionNonce,
            Player,
            new[] { Stack("Stone", "Stone", slot: 1) },
            Now));
        cache.SwitchWorld(new string("world-a".ToCharArray()));
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(Player.GameId, Now, out _),
            "Equivalent world value keys must preserve inventory.");

        cache.SwitchWorld("world-b");
        AssertAllAliasesAreMissing(cache, Player);
    }

    [TestMethod]
    public void OldSessionCannotOverwriteNewerSession()
    {
        var cache = new CompanionInventoryCache();
        cache.BeginSession(PeerId, "session-old");
        Assert.IsTrue(cache.Remember(
            PeerId,
            "session-old",
            Player,
            new[] { Stack("Wood", "Wood", amount: 10, slot: 0) },
            Now));

        cache.BeginSession(PeerId, "session-new");

        Assert.AreEqual(
            CompanionInventoryState.Missing,
            cache.TryGet(Player.GameId, Now.AddSeconds(1), out _),
            "Beginning a replacement session must invalidate the previous session snapshot.");
        Assert.IsFalse(cache.Remember(
            PeerId,
            "session-old",
            Player,
            new[] { Stack("Wood", "Wood", amount: 99, slot: 0) },
            Now.AddSeconds(2)));
        Assert.IsTrue(cache.Remember(
            PeerId,
            "session-new",
            Player,
            new[] { Stack("Stone", "Stone", amount: 1, slot: 1) },
            Now.AddSeconds(1)));

        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(Player.GameId, Now.AddSeconds(2), out var items));
        Assert.AreEqual("Stone", items.Single().Code);
    }

    [TestMethod]
    public void InventoryOutputIsCanonicalAndCannotMutateCachedCollection()
    {
        var cache = CreateCache();
        var source = new List<CompanionInventoryStack>
        {
            Stack(" Stone ", " Stone Name ", amount: 2, quality: 3, durability: 50, equipped: true, slot: 1),
            Stack(" Wood ", " Wood Name ", amount: 1, quality: 2, durability: 75, slot: 0)
        };
        Assert.IsTrue(cache.Remember(PeerId, SessionNonce, Player, source, Now));

        source.Clear();
        Assert.AreEqual(
            CompanionInventoryState.Fresh,
            cache.TryGet(Player.GameId, Now, out var items));
        Assert.AreEqual(2, items.Count);
        Assert.AreEqual("Wood", items[0].Code);
        Assert.AreEqual("Wood Name", items[0].Name);
        Assert.AreEqual("2", items[0].Quality);
        var position = items[0].Position;
        Assert.IsNotNull(position);
        Assert.AreEqual(0, position.X);
        Assert.AreEqual(0, position.Y);
        Assert.AreEqual("Stone", items[1].Code);
        Assert.AreEqual(true, items[1].Equipped);

        var mutableView = (IList<TakaroInventoryItem>)items;
        Assert.ThrowsException<NotSupportedException>(() => mutableView[0] =
            new TakaroInventoryItem("Tampered", "Tampered", 1, "1"));
        Assert.AreEqual(
            "Wood",
            cache.TryGet(Player.GameId, Now, out var reread) == CompanionInventoryState.Fresh
                ? reread[0].Code
                : string.Empty);
    }

    [TestMethod]
    public async Task ParallelInventoryLifecycleOperationsRemainSafe()
    {
        var cache = new CompanionInventoryCache(TimeSpan.FromSeconds(30));
        cache.BeginSession(PeerId, SessionNonce);
        var start = new ManualResetEventSlim(false);
        var workers = Enumerable.Range(0, 8).Select(worker => Task.Run(() =>
        {
            start.Wait();
            for (var iteration = 0; iteration < 2_000; iteration++)
            {
                var player = Player with { Name = $"Odin-{worker}-{iteration}" };
                cache.Remember(
                    PeerId,
                    SessionNonce,
                    player,
                    new[] { Stack("Wood", "Wood", slot: iteration % CompanionProtocol.MaximumInventoryStacks) },
                    Now.AddMilliseconds(iteration));
                cache.TryGet(player.GameId, Now.AddMilliseconds(iteration), out _);
            }
        })).ToArray();

        start.Set();
        await Task.WhenAll(workers);

        cache.Clear();
        Assert.AreEqual(CompanionInventoryState.Missing, cache.TryGet(Player.GameId, Now, out _));
    }

    private static CompanionInventoryCache CreateCache()
    {
        var cache = new CompanionInventoryCache(TimeSpan.FromSeconds(30));
        cache.BeginSession(PeerId, SessionNonce);
        return cache;
    }

    private static CompanionInventoryStack Stack(
        string code,
        string name,
        int amount = 1,
        int quality = 1,
        float durability = 100,
        bool equipped = false,
        int slot = 0) =>
        new(code, name, amount, quality, durability, equipped, slot);

    private static void AssertAllAliasesAreMissing(
        CompanionInventoryCache cache,
        TakaroPlayer player)
    {
        foreach (var alias in new[] { player.GameId, player.PlatformId, player.SteamId, player.Name })
        {
            Assert.AreEqual(
                CompanionInventoryState.Missing,
                cache.TryGet(alias!, Now, out _),
                alias);
        }
    }
}
