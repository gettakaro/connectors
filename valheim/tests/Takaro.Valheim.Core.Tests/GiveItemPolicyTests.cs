using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class GiveItemPolicyTests
{
    [TestMethod]
    public void PlansWorldDropsUsingThePrefabStackSize()
    {
        var result = GiveItemPolicy.PlanStacks(amount: 101, maxStackSize: 50);

        Assert.IsTrue(result.Success);
        CollectionAssert.AreEqual(new[] { 50, 50, 1 }, result.Stacks.ToArray());
        Assert.IsNull(result.ErrorCode);
    }

    [DataTestMethod]
    [DataRow(0)]
    [DataRow(-1)]
    [DataRow(GiveItemPolicy.MaxAmount + 1)]
    [DataRow(int.MaxValue)]
    public void RejectsAmountsOutsideTheStrictRequestBound(int amount)
    {
        var result = GiveItemPolicy.PlanStacks(amount, maxStackSize: 50);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("invalid_amount", result.ErrorCode);
        StringAssert.Contains(result.ErrorMessage, $"between 1 and {GiveItemPolicy.MaxAmount}");
        Assert.AreEqual(0, result.Stacks.Count);
    }

    [TestMethod]
    public void AcceptsTheMaximumBoundedAmountWhenItFitsTheDropLimit()
    {
        var result = GiveItemPolicy.PlanStacks(GiveItemPolicy.MaxAmount, maxStackSize: 50);

        Assert.IsTrue(result.Success);
        Assert.AreEqual(20, result.Stacks.Count);
        Assert.AreEqual(GiveItemPolicy.MaxAmount, result.Stacks.Sum());
    }

    [TestMethod]
    public void RejectsRequestsThatWouldCreateTooManyWorldDrops()
    {
        var result = GiveItemPolicy.PlanStacks(
            amount: GiveItemPolicy.MaxDropStacks + 1,
            maxStackSize: 1);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("item_drop_limit_exceeded", result.ErrorCode);
        StringAssert.Contains(result.ErrorMessage, GiveItemPolicy.MaxDropStacks.ToString());
        Assert.AreEqual(0, result.Stacks.Count);
    }

    [TestMethod]
    public void TreatsAnUnknownPrefabStackSizeAsOneDrop()
    {
        var result = GiveItemPolicy.PlanStacks(amount: 20, maxStackSize: 0);

        Assert.IsTrue(result.Success);
        CollectionAssert.AreEqual(new[] { 20 }, result.Stacks.ToArray());
    }
}
