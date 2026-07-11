using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionInventoryReaderContractTests
{
    [TestMethod]
    public void InitialReadySnapshotIsChangedAndUnchangedSnapshotIsNotResent()
    {
        var reader = new CompanionInventoryReader();
        var items = new[] { Item("Wood", "Wood", amount: 12, slot: 3) };

        Assert.IsTrue(reader.TryGetChanged(items, out var initial));
        Assert.IsNotNull(initial);
        reader.MarkSent(initial);
        Assert.IsFalse(reader.TryGetChanged(items, out _));
    }

    [TestMethod]
    public void CanonicalOrderingProducesStableFingerprint()
    {
        var forward = new CompanionInventoryReader();
        var reverse = new CompanionInventoryReader();
        var first = Item("Wood", "Wood", amount: 12, slot: 3);
        var second = Item("Stone", "Stone", amount: 5, slot: 1);

        Assert.IsTrue(forward.TryGetChanged(new[] { first, second }, out var a));
        Assert.IsTrue(reverse.TryGetChanged(new[] { second, first }, out var b));
        Assert.IsNotNull(a);
        Assert.IsNotNull(b);

        Assert.AreEqual(a.Fingerprint, b.Fingerprint);
        CollectionAssert.AreEqual(
            new[] { "Stone", "Wood" },
            a.Stacks.Select(stack => stack.Code).ToArray());
        CollectionAssert.AreEqual(
            a.Stacks.ToArray(),
            b.Stacks.ToArray());
    }

    [TestMethod]
    public void MutationIsSentAfterUnchangedSnapshot()
    {
        var reader = new CompanionInventoryReader();
        Assert.IsTrue(reader.TryGetChanged(
            new[] { Item("Wood", "Wood", amount: 1, slot: 0) },
            out var initial));
        Assert.IsNotNull(initial);
        reader.MarkSent(initial);

        Assert.IsTrue(reader.TryGetChanged(
            new[] { Item("Wood", "Wood", amount: 2, slot: 0) },
            out var changed));
        Assert.IsNotNull(changed);
        Assert.AreNotEqual(initial.Fingerprint, changed.Fingerprint);
    }

    [TestMethod]
    public void UnchangedSnapshotIsRefreshedBeforeServerCacheCanExpire()
    {
        var reader = new CompanionInventoryReader();
        var items = new[] { Item("Wood", "Wood", amount: 1, slot: 0) };
        var refreshInterval = TimeSpan.FromSeconds(20);

        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            items,
            TimeSpan.Zero,
            refreshInterval,
            out var initial));
        Assert.IsNotNull(initial);
        reader.MarkSent(initial, TimeSpan.Zero);

        Assert.IsFalse(reader.TryGetChangedOrRefresh(
            items,
            refreshInterval - TimeSpan.FromTicks(1),
            refreshInterval,
            out _));
        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            items,
            refreshInterval,
            refreshInterval,
            out var refresh));
        Assert.IsNotNull(refresh);
        Assert.AreEqual(initial.Fingerprint, refresh.Fingerprint);
    }

    [TestMethod]
    public void FailedRefreshRemainsDueUntilItIsMarkedSent()
    {
        var reader = new CompanionInventoryReader();
        var items = new[] { Item("Wood", "Wood", amount: 1, slot: 0) };
        var refreshInterval = TimeSpan.FromSeconds(20);

        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            items,
            TimeSpan.Zero,
            refreshInterval,
            out var initial));
        Assert.IsNotNull(initial);
        reader.MarkSent(initial, TimeSpan.Zero);

        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            items,
            refreshInterval,
            refreshInterval,
            out _));
        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            items,
            refreshInterval + TimeSpan.FromSeconds(2),
            refreshInterval,
            out var retry));
        Assert.IsNotNull(retry);
        reader.MarkSent(retry, refreshInterval + TimeSpan.FromSeconds(2));
        Assert.IsFalse(reader.TryGetChangedOrRefresh(
            items,
            refreshInterval + TimeSpan.FromSeconds(3),
            refreshInterval,
            out _));
    }

    [TestMethod]
    public void ConfirmedEmptyInventoryIsARealSnapshot()
    {
        var reader = new CompanionInventoryReader();

        Assert.IsFalse(reader.TryGetChanged(null, out _));
        Assert.IsTrue(reader.TryGetChanged(
            Array.Empty<CompanionInventorySourceItem>(),
            out var empty));
        Assert.IsNotNull(empty);
        Assert.AreEqual(0, empty.Stacks.Count);
        reader.MarkSent(empty);
        Assert.IsFalse(reader.TryGetChanged(
            Array.Empty<CompanionInventorySourceItem>(),
            out _));
    }

    [TestMethod]
    public void SnapshotIsBoundedToProtocolLimits()
    {
        var reader = new CompanionInventoryReader();
        var items = Enumerable.Range(0, CompanionProtocol.MaximumInventoryStacks + 40)
            .Select(index => Item(
                new string('c', CompanionProtocol.MaximumCodeCharacters + 20),
                new string('n', CompanionProtocol.MaximumChatCharacters + 20),
                amount: CompanionProtocol.MaximumInventoryAmount + 20,
                slot: index,
                quality: CompanionProtocol.MaximumItemQuality + 20,
                durability: float.PositiveInfinity))
            .ToArray();

        Assert.IsTrue(reader.TryGetChanged(items, out var snapshot));
        Assert.IsNotNull(snapshot);
        Assert.IsTrue(snapshot.Stacks.Count <= CompanionProtocol.MaximumInventoryStacks);
        foreach (var stack in snapshot.Stacks)
        {
            Assert.IsTrue(stack.Code.Length <= CompanionProtocol.MaximumCodeCharacters);
            Assert.IsTrue(stack.Name.Length <= CompanionProtocol.MaximumChatCharacters);
            Assert.IsTrue(stack.Amount >= 1 && stack.Amount <= CompanionProtocol.MaximumInventoryAmount);
            Assert.IsTrue(stack.Quality >= 1 && stack.Quality <= CompanionProtocol.MaximumItemQuality);
            Assert.IsTrue(stack.Durability >= 0 && stack.Durability <= CompanionProtocol.MaximumDurability);
            Assert.IsTrue(stack.Slot >= 0 && stack.Slot <= CompanionProtocol.MaximumInventorySlot);
        }
    }

    [TestMethod]
    public void ResetMakesCurrentSnapshotImmediatelySendableAgain()
    {
        var reader = new CompanionInventoryReader();
        var items = new[] { Item("Wood", "Wood", amount: 1, slot: 0) };
        Assert.IsTrue(reader.TryGetChanged(items, out var initial));
        Assert.IsNotNull(initial);
        reader.MarkSent(initial);
        Assert.IsFalse(reader.TryGetChanged(items, out _));

        reader.Reset();

        Assert.IsTrue(reader.TryGetChanged(items, out _));
    }

    private static CompanionInventorySourceItem Item(
        string code,
        string name,
        int amount,
        int slot,
        int quality = 1,
        float durability = 100,
        bool equipped = false) =>
        new(code, name, amount, quality, durability, equipped, slot);
}
