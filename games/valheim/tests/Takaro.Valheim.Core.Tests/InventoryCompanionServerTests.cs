using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class InventoryCompanionServerTests
{
    private const long PeerA = 1001;
    private const long PeerB = 2002;
    private const string NonceA = "nonce-a";
    private const string NonceB = "nonce-b";
    private static readonly DateTimeOffset Now = DateTimeOffset.Parse("2026-10-02T10:00:00+00:00");
    private static readonly TakaroPlayer PlayerA = new("Steam_76561190000000001", "Alice", null, "Steam_76561190000000001", null, null);
    private static readonly TakaroPlayer PlayerB = new("Steam_76561190000000002", "Bob", null, "Steam_76561190000000002", null, null);

    [TestMethod]
    public void ProtocolTwoCompanionNackIsRejectedAndLatchedWithoutAnyKick()
    {
        var server = new Server();
        server.Begin(PeerA, NonceA);

        var nack = CompanionProtocolTests.EnvelopeJson(
            2,
            CompanionMessageTypes.HelloNack,
            """{"minimumVersion":2,"maximumVersion":2,"productVersion":"3.0.2"}""",
            NonceA,
            sequence: 1);
        var result = server.Handle(PeerA, PlayerA, nack);

        Assert.AreEqual(CompanionMessageOutcome.VersionRejected, result.Outcome);
        Assert.AreEqual(2, result.RemoteMinimumVersion);
        Assert.AreEqual(2, result.RemoteMaximumVersion);
        Assert.AreEqual("3.0.2", result.RemoteProductVersion);
        Assert.IsTrue(server.Sessions.TryGetSnapshot(PeerA, out var snapshot));
        Assert.IsTrue(snapshot.IsRejected);
        Assert.IsFalse(server.Sessions.IsActive(PeerA, Now));

        // A latched rejection cannot be turned into a session afterwards.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerA, PlayerA, HelloAck(NonceA, 3, sequence: 2)).Outcome);
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerA, PlayerA, Inventory(NonceA, sequence: 3)).Outcome);
        Assert.AreEqual(
            CompanionInventoryActionPolicy.UnsupportedErrorCode,
            CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now).ErrorCode);
    }

    [TestMethod]
    public void NewerCompanionAckIsRejectedWithItsVersion()
    {
        var server = new Server();
        server.Begin(PeerA, NonceA);

        var result = server.Handle(PeerA, PlayerA, HelloAck(NonceA, 4, sequence: 1, envelopeVersion: 4));

        Assert.AreEqual(CompanionMessageOutcome.VersionRejected, result.Outcome);
        Assert.AreEqual(4, result.RemoteMaximumVersion);
        Assert.IsFalse(server.Sessions.IsActive(PeerA, Now));
    }

    [TestMethod]
    public void OverlappingNackIsIgnoredAsConfused()
    {
        var server = new Server();
        server.Begin(PeerA, NonceA);
        var nack = CompanionProtocolTests.EnvelopeJson(
            3,
            CompanionMessageTypes.HelloNack,
            """{"minimumVersion":2,"maximumVersion":4,"productVersion":"x"}""",
            NonceA,
            sequence: 1);

        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerA, PlayerA, nack).Outcome);
        Assert.IsFalse(server.Sessions.TryGetSnapshot(PeerA, out var snapshot) && snapshot.IsRejected);
    }

    [TestMethod]
    public void NegotiatedSessionAcceptsSnapshotsAndAnswersInventory()
    {
        var server = new Server();
        server.Negotiate(PeerA, NonceA);

        var result = server.Handle(PeerA, PlayerA, Inventory(NonceA, sequence: 2, ("Wood", "$item_wood", 12), ("AxeStone", "$item_axe_stone", 1)));

        Assert.AreEqual(CompanionMessageOutcome.InventoryAccepted, result.Outcome);
        Assert.AreEqual(2, result.InventoryStacks);
        var answer = CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now);
        Assert.IsTrue(answer.Success);
        var items = ((IEnumerable<TakaroInventoryItem>)answer.Payload!).ToArray();
        Assert.AreEqual(2, items.Length);
        Assert.AreEqual("Wood", items[0].Code);
        Assert.AreEqual(12, items[0].Amount);
    }

    [TestMethod]
    public void SessionIsBoundToThePeerItWasOfferedTo()
    {
        var server = new Server();
        server.Negotiate(PeerA, NonceA);
        server.Begin(PeerB, NonceB);

        // Peer B replays peer A's nonce: refused, and nothing lands in either inventory.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerB, PlayerB, Inventory(NonceA, sequence: 5, ("Coins", "$item_coins", 999))).Outcome);
        // Peer B before negotiating: refused.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerB, PlayerB, Inventory(NonceB, sequence: 2, ("Coins", "$item_coins", 999))).Outcome);
        // A peer that was never offered a session: refused.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(3003, PlayerB, HelloAck(NonceA, 3, sequence: 1)).Outcome);

        Assert.IsFalse(CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now).Success);
        Assert.IsFalse(CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerB, server.Inventory, Now).Success);
        Assert.IsFalse(server.Sessions.IsActive(PeerB, Now));

        // The snapshot is stored under the authenticated peer's player, whatever it contains.
        Assert.AreEqual(CompanionMessageOutcome.InventoryAccepted, server.Handle(PeerA, PlayerA, Inventory(NonceA, sequence: 6, ("Stone", "$item_stone", 3))).Outcome);
        Assert.IsTrue(CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now).Success);
        Assert.IsFalse(CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerB, server.Inventory, Now).Success);
    }

    [TestMethod]
    public void NoCompanionMeansAnErrorNeverAnEmptyList()
    {
        var cache = new CompanionInventoryCache();

        var result = CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, cache, Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("server_only_unsupported", result.ErrorCode);
        Assert.IsNull(result.Payload);
        Assert.AreEqual("player_not_found", CompanionInventoryActionPolicy.FromResolvedPlayer(null, cache, Now).ErrorCode);
    }

    [TestMethod]
    public void StaleSnapshotIsAnErrorButAConfirmedEmptySnapshotIsAnEmptyList()
    {
        var server = new Server();
        server.Negotiate(PeerA, NonceA);
        Assert.AreEqual(CompanionMessageOutcome.InventoryAccepted, server.Handle(PeerA, PlayerA, Inventory(NonceA, sequence: 2)).Outcome);

        var fresh = CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now.AddSeconds(29));
        Assert.IsTrue(fresh.Success);
        Assert.AreEqual(0, ((IEnumerable<TakaroInventoryItem>)fresh.Payload!).Count());

        var stale = CompanionInventoryActionPolicy.FromResolvedPlayer(PlayerA, server.Inventory, Now.AddSeconds(31));
        Assert.IsFalse(stale.Success);
        Assert.AreEqual(CompanionInventoryActionPolicy.StaleErrorCode, stale.ErrorCode);
        Assert.IsNull(stale.Payload);
    }

    [TestMethod]
    public void KillVerdictIsStoredForItsPeerAndTakenOnceWithinTheWindow()
    {
        var server = new Server();
        server.Negotiate(PeerA, NonceA);

        var result = server.Handle(PeerA, PlayerA, Verdict(NonceA, sequence: 2, "55:7", "$enemy_boar", CompanionAttackerKind.Creature), receivedAt: Now);
        Assert.AreEqual(CompanionMessageOutcome.KillVerdictAccepted, result.Outcome);
        Assert.AreEqual("55:7", result.KillVerdict!.CreatureZdo);

        Assert.IsFalse(server.Verdicts.TryTake(PeerB, null, "$enemy_boar", Now, TimeSpan.FromSeconds(5), out _), "another peer");
        Assert.IsFalse(server.Verdicts.TryTake(PeerA, null, "$enemy_neck", Now, TimeSpan.FromSeconds(5), out _), "another creature");
        Assert.IsFalse(server.Verdicts.TryTake(PeerA, null, "$enemy_boar", Now.AddSeconds(6), TimeSpan.FromSeconds(5), out _), "outside window");
        Assert.IsTrue(server.Verdicts.TryTake(PeerA, null, "$enemy_boar", Now.AddSeconds(-4), TimeSpan.FromSeconds(5), out var taken));
        Assert.IsFalse(taken!.LastHitByLocalPlayer);
        Assert.AreEqual(CompanionAttackerKind.Creature, taken.LastHitAttackerKind);
        Assert.IsFalse(server.Verdicts.TryTake(PeerA, null, "$enemy_boar", Now, TimeSpan.FromSeconds(5), out _), "consumed");
    }

    [TestMethod]
    public void TryTakePrefersTheExactCreatureThenTheClosestInTime()
    {
        var store = new CompanionKillVerdictStore();
        store.Add(PeerA, new CompanionKillVerdict("1:1", "Boar", "$enemy_boar", true, CompanionAttackerKind.LocalPlayer), Now);
        store.Add(PeerA, new CompanionKillVerdict("1:2", "Boar", "$enemy_boar", false, CompanionAttackerKind.OtherPlayer), Now.AddSeconds(2));

        Assert.IsTrue(store.TryTake(PeerA, "1:1", "$enemy_boar", Now.AddSeconds(2), TimeSpan.FromSeconds(5), out var exact));
        Assert.AreEqual("1:1", exact!.CreatureZdo);
        Assert.IsTrue(store.TryTake(PeerA, null, "$enemy_boar", Now.AddSeconds(2), TimeSpan.FromSeconds(5), out var closest));
        Assert.AreEqual("1:2", closest!.CreatureZdo);
        Assert.AreEqual(0, store.Count(PeerA));
    }

    [TestMethod]
    public void KillVerdictStoreIsBoundedAndAgesOut()
    {
        var store = new CompanionKillVerdictStore(TimeSpan.FromSeconds(30), maximumPerPeer: 3, maximumPeers: 1);
        for (var i = 0; i < 5; i++)
        {
            Assert.IsTrue(store.Add(PeerA, new CompanionKillVerdict($"1:{i}", "Neck", "$enemy_neck", true, CompanionAttackerKind.LocalPlayer), Now));
        }

        Assert.AreEqual(3, store.Count(PeerA));
        Assert.IsFalse(store.Add(PeerB, new CompanionKillVerdict("2:1", "Neck", "$enemy_neck", true, CompanionAttackerKind.LocalPlayer), Now), "peer cap");
        store.Add(PeerA, new CompanionKillVerdict("1:99", "Neck", "$enemy_neck", true, CompanionAttackerKind.LocalPlayer), Now.AddMinutes(1));
        Assert.AreEqual(1, store.Count(PeerA));
        store.RemovePeer(PeerA);
        Assert.AreEqual(0, store.Count(PeerA));
    }

    [TestMethod]
    public void KillVerdictsAreRateLimitedPerPeer()
    {
        var server = new Server();
        server.Negotiate(PeerA, NonceA);
        var accepted = 0;
        for (var sequence = 2; sequence < 30; sequence++)
        {
            if (server.Handle(PeerA, PlayerA, Verdict(NonceA, sequence, $"9:{sequence}", "$enemy_deer", CompanionAttackerKind.LocalPlayer)).Outcome
                == CompanionMessageOutcome.KillVerdictAccepted)
            {
                accepted++;
            }
        }

        Assert.AreEqual(8, accepted);
    }

    [TestMethod]
    public void FacadeAnswersOnlyForAnAttachedBridgeAndActiveSessions()
    {
        var server = new Server();
        Assert.IsFalse(CompanionKillVerdicts.HasSession(PeerA));

        CompanionKillVerdicts.Attach(server.Sessions, server.Verdicts, () => Now);
        try
        {
            server.Begin(PeerA, NonceA);
            Assert.IsFalse(CompanionKillVerdicts.HasSession(PeerA), "offered but not negotiated");
            Assert.AreEqual(CompanionMessageOutcome.Negotiated, server.Handle(PeerA, PlayerA, HelloAck(NonceA, 3, sequence: 1)).Outcome);
            Assert.IsTrue(CompanionKillVerdicts.HasSession(PeerA));
            Assert.IsFalse(CompanionKillVerdicts.HasSession(PeerB));

            server.Handle(PeerA, PlayerA, Verdict(NonceA, sequence: 2, "4:4", "$enemy_greyling", CompanionAttackerKind.LocalPlayer), receivedAt: Now);
            Assert.IsTrue(CompanionKillVerdicts.TryTake(PeerA, "$enemy_greyling", Now.AddSeconds(1), TimeSpan.FromSeconds(3), out var byPlayer, out var kind));
            Assert.IsTrue(byPlayer);
            Assert.AreEqual(CompanionAttackerKind.LocalPlayer, kind);
            Assert.IsFalse(CompanionKillVerdicts.TryTake(PeerA, "$enemy_greyling", Now.AddSeconds(1), TimeSpan.FromSeconds(3), out _, out kind));
            Assert.AreEqual(CompanionAttackerKind.None, kind);
        }
        finally
        {
            CompanionKillVerdicts.Detach(server.Sessions);
        }

        Assert.IsFalse(CompanionKillVerdicts.HasSession(PeerA));
    }

    private static string HelloAck(string nonce, int version, long sequence, int? envelopeVersion = null) =>
        CompanionProtocolTests.EnvelopeJson(
            envelopeVersion ?? version,
            CompanionMessageTypes.HelloAck,
            $$"""{"protocolVersion":{{version}},"productVersion":"3.1.0","acceptedCapabilities":2}""",
            nonce,
            sequence);

    private static string Inventory(string nonce, long sequence, params (string Code, string Name, int Amount)[] stacks) =>
        CompanionProtocolTests.EnvelopeJson(
            3,
            CompanionMessageTypes.InventorySnapshot,
            "{\"stacks\":[" + string.Join(",", stacks.Select((stack, slot) =>
                $$"""{"code":"{{stack.Code}}","name":"{{stack.Name}}","amount":{{stack.Amount}},"quality":1,"durability":0,"equipped":false,"slot":{{slot}}}""")) + "]}",
            nonce,
            sequence);

    private static string Verdict(string nonce, long sequence, string zdo, string token, string kind) =>
        CompanionProtocolTests.EnvelopeJson(
            3,
            CompanionMessageTypes.KillVerdict,
            $$"""{"creatureZdo":"{{zdo}}","prefab":"X","enemyToken":"{{token}}","lastHitByLocalPlayer":{{(kind == CompanionAttackerKind.LocalPlayer ? "true" : "false")}},"lastHitAttackerKind":"{{kind}}"}""",
            nonce,
            sequence);

    private sealed class Server
    {
        public Server()
        {
            Sessions = new CompanionSessionRegistry(3, 3, CompanionCapability.Inventory, TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(30));
            Inventory = new CompanionInventoryCache();
            Verdicts = new CompanionKillVerdictStore();
            Handler = new CompanionServerMessageHandler(
                Sessions,
                new CompanionRateLimiter(10, 5, TimeSpan.FromSeconds(1)),
                new CompanionRateLimiter(8, 4, TimeSpan.FromSeconds(1)),
                Inventory,
                Verdicts);
        }

        public CompanionSessionRegistry Sessions { get; }

        public CompanionInventoryCache Inventory { get; }

        public CompanionKillVerdictStore Verdicts { get; }

        public CompanionServerMessageHandler Handler { get; }

        public void Begin(long peer, string nonce)
        {
            Sessions.Begin(peer, Now, nonce);
            Inventory.BeginSession(peer, nonce);
        }

        public void Negotiate(long peer, string nonce)
        {
            Begin(peer, nonce);
            Assert.AreEqual(CompanionMessageOutcome.Negotiated, Handle(peer, peer == PeerA ? PlayerA : PlayerB, HelloAck(nonce, 3, sequence: 1)).Outcome);
        }

        public CompanionMessageHandlingResult Handle(long peer, TakaroPlayer player, string json, DateTimeOffset? receivedAt = null) =>
            Handler.Handle(peer, player, json, Now, receivedAt ?? Now);
    }
}
