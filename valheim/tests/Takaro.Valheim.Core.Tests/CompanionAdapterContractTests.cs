using System.Reflection;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionAdapterContractTests
{
    private const long PeerId = 42;
    private const string SessionNonce = "adapter-contract";

    private static readonly DateTimeOffset Now =
        DateTimeOffset.Parse("2026-07-11T10:00:00+02:00");

    private static readonly TakaroPlayer ResolvedPlayer =
        new("Steam_1", "Odin", "1", "steam:1", null, null);

    [DataTestMethod]
    [DataRow("Required")]
    [DataRow("Optional")]
    public void FreshNonemptyInventorySucceedsForEnabledCompanionModes(string mode)
    {
        var cache = CacheWithSnapshot(
            ResolvedPlayer,
            new[] { new CompanionInventoryStack("SwordIron", "Iron sword", 1, 2, 93.5f, true, 4) });

        var result = InvokePolicy(mode, ResolvedPlayer, cache, Now.AddSeconds(29));

        Assert.IsTrue(result.Success);
        var item = AssertItems(result).Single();
        Assert.AreEqual("SwordIron", item.Code);
        Assert.AreEqual("Iron sword", item.Name);
        Assert.AreEqual(1, item.Amount);
        Assert.AreEqual("2", item.Quality);
    }

    [TestMethod]
    public void ConfirmedFreshEmptyInventorySucceeds()
    {
        var cache = CacheWithSnapshot(ResolvedPlayer, Array.Empty<CompanionInventoryStack>());

        var result = InvokePolicy("Required", ResolvedPlayer, cache, Now.AddSeconds(29));

        Assert.IsTrue(result.Success);
        Assert.AreEqual(0, AssertItems(result).Count);
    }

    [TestMethod]
    public void MissingInventoryIsUnavailableInsteadOfFabricatedEmptySuccess()
    {
        var result = InvokePolicy(
            "Required",
            ResolvedPlayer,
            new CompanionInventoryCache(TimeSpan.FromSeconds(30)),
            Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_component_unavailable", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void ExpiredInventoryIsUnavailableInsteadOfFabricatedEmptySuccess()
    {
        var cache = CacheWithSnapshot(
            ResolvedPlayer,
            new[] { new CompanionInventoryStack("Wood", "Wood", 10, 1, 100, false, 0) });

        var result = InvokePolicy("Required", ResolvedPlayer, cache, Now.AddSeconds(30));

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_component_unavailable", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void DisabledCompanionModeIsUnavailableEvenWithFreshInventory()
    {
        var cache = CacheWithSnapshot(
            ResolvedPlayer,
            new[] { new CompanionInventoryStack("Wood", "Wood", 10, 1, 100, false, 0) });

        var result = InvokePolicy("Disabled", ResolvedPlayer, cache, Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_component_unavailable", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void UnresolvedServerPlayerReturnsPlayerNotFound()
    {
        var result = InvokePolicy(
            "Required",
            player: null,
            new CompanionInventoryCache(TimeSpan.FromSeconds(30)),
            Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_not_found", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void InventoryLookupUsesOnlyAliasesFromAuthoritativeResolvedPlayer()
    {
        var callerControlledIdentity = new TakaroPlayer(
            "caller-input",
            "Caller Input",
            null,
            null,
            null,
            null);
        var cache = CacheWithSnapshot(
            callerControlledIdentity,
            new[] { new CompanionInventoryStack("Coins", "Coins", 99, 1, 100, false, 0) });

        var result = InvokePolicy("Required", ResolvedPlayer, cache, Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_component_unavailable", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void StableIdentifierCannotFallThroughToAnotherPlayersMatchingDisplayName()
    {
        var resolvedPlayer = new TakaroPlayer(
            "resolved-game-id",
            "Resolved Viking",
            "resolved-steam-id",
            "steam:resolved-steam-id",
            null,
            null);
        var otherPlayer = new TakaroPlayer(
            "other-game-id",
            resolvedPlayer.GameId,
            "other-steam-id",
            "steam:other-steam-id",
            null,
            null);
        var cache = CacheWithSnapshot(
            otherPlayer,
            new[] { new CompanionInventoryStack("Coins", "Coins", 99, 1, 100, false, 0) });

        var result = InvokePolicy("Required", resolvedPlayer, cache, Now);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_component_unavailable", result.ErrorCode);
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void ResolvedPlayersStableSnapshotWinsOverAnotherPlayersMatchingDisplayName()
    {
        var resolvedPlayer = new TakaroPlayer(
            "resolved-game-id",
            "Resolved Viking",
            "resolved-steam-id",
            "steam:resolved-steam-id",
            null,
            null);
        var otherPlayer = new TakaroPlayer(
            "other-game-id",
            resolvedPlayer.GameId,
            "other-steam-id",
            "steam:other-steam-id",
            null,
            null);
        var cache = new CompanionInventoryCache(TimeSpan.FromSeconds(30));
        RememberSnapshot(
            cache,
            peerId: 1,
            sessionNonce: "other-session",
            otherPlayer,
            new[] { new CompanionInventoryStack("Coins", "Coins", 99, 1, 100, false, 0) });
        RememberSnapshot(
            cache,
            peerId: 2,
            sessionNonce: "resolved-session",
            resolvedPlayer,
            new[] { new CompanionInventoryStack("Wood", "Wood", 10, 1, 100, false, 0) });

        var result = InvokePolicy("Required", resolvedPlayer, cache, Now);

        Assert.IsTrue(result.Success);
        Assert.AreEqual("Wood", AssertItems(result).Single().Code);
    }

    private static CompanionInventoryCache CacheWithSnapshot(
        TakaroPlayer player,
        IReadOnlyList<CompanionInventoryStack> stacks)
    {
        var cache = new CompanionInventoryCache(TimeSpan.FromSeconds(30));
        RememberSnapshot(cache, PeerId, SessionNonce, player, stacks);
        return cache;
    }

    private static void RememberSnapshot(
        CompanionInventoryCache cache,
        long peerId,
        string sessionNonce,
        TakaroPlayer player,
        IReadOnlyList<CompanionInventoryStack> stacks)
    {
        cache.BeginSession(peerId, sessionNonce);
        Assert.IsTrue(cache.Remember(peerId, sessionNonce, player, stacks, Now));
    }

    private static TakaroActionResult InvokePolicy(
        string mode,
        TakaroPlayer? player,
        CompanionInventoryCache cache,
        DateTimeOffset now)
    {
        var coreAssembly = typeof(ConnectorConfig).Assembly;
        var modeType = coreAssembly.GetType("Takaro.Valheim.Core.CompanionMode")
            ?? throw new AssertFailedException("Core is missing CompanionMode.");
        var policyType = coreAssembly.GetType("Takaro.Valheim.Core.CompanionInventoryActionPolicy")
            ?? throw new AssertFailedException("Core is missing CompanionInventoryActionPolicy.");
        var method = policyType.GetMethod(
            "FromResolvedPlayer",
            BindingFlags.Public | BindingFlags.Static)
            ?? throw new AssertFailedException("CompanionInventoryActionPolicy is missing FromResolvedPlayer.");
        var parsedMode = Enum.Parse(modeType, mode);

        return method.Invoke(null, new object?[] { parsedMode, player, cache, now }) as TakaroActionResult
            ?? throw new AssertFailedException("CompanionInventoryActionPolicy returned no TakaroActionResult.");
    }

    private static IReadOnlyList<TakaroInventoryItem> AssertItems(TakaroActionResult result) =>
        result.Payload as IReadOnlyList<TakaroInventoryItem>
        ?? throw new AssertFailedException("Expected an inventory item list payload.");
}
