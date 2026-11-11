using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ReconnectPolicyTests
{
    private static readonly TimeSpan Unit = ReconnectPolicy.DefaultBackoffUnit;

    [TestMethod]
    public void TheFirstRetryAfterAStableSessionIsUnderASecondWithJitter()
    {
        var low = ReconnectPolicy.NextDelay(0, Unit, 0);
        var high = ReconnectPolicy.NextDelay(0, Unit, 0.999);
        Assert.AreEqual(TimeSpan.FromMilliseconds(300), low);
        Assert.IsTrue(high > low, "jitter spreads the fast retry");
        Assert.IsTrue(high < TimeSpan.FromSeconds(1), $"fast retry {high} must leave room for TLS and identify within 2 s");
    }

    [TestMethod]
    public void TheFastRetryIgnoresAConfiguredBackoffUnit()
    {
        Assert.AreEqual(
            ReconnectPolicy.NextDelay(0, TimeSpan.FromSeconds(30), 0.5),
            ReconnectPolicy.NextDelay(0, TimeSpan.FromSeconds(1), 0.5));
    }

    [TestMethod]
    public void ConsecutiveFailuresDoubleTheCeiling()
    {
        double[] expectedCeilings = { 2, 4, 8, 16, 32 };
        for (var streak = 1; streak <= expectedCeilings.Length; streak++)
        {
            var ceiling = expectedCeilings[streak - 1];
            Assert.AreEqual(ceiling / 2, ReconnectPolicy.NextDelay(streak, Unit, 0).TotalSeconds, 1e-9, $"streak {streak} floor");
            Assert.AreEqual(ceiling, ReconnectPolicy.NextDelay(streak, Unit, 1).TotalSeconds, 1e-9, $"streak {streak} ceiling");
        }
    }

    [TestMethod]
    public void TheBackoffIsCappedAtSixtySecondsAndStaysJittered()
    {
        foreach (var streak in new[] { 6, 7, 20, 1000, int.MaxValue })
        {
            Assert.AreEqual(60, ReconnectPolicy.NextDelay(streak, Unit, 1).TotalSeconds, 1e-9);
            Assert.AreEqual(30, ReconnectPolicy.NextDelay(streak, Unit, 0).TotalSeconds, 1e-9);
        }

        Assert.AreEqual(60, ReconnectPolicy.NextDelay(1, TimeSpan.FromMinutes(5), 1).TotalSeconds, 1e-9, "a huge unit is capped too");
    }

    [TestMethod]
    public void JitterIsClampedToItsRange()
    {
        Assert.AreEqual(ReconnectPolicy.NextDelay(3, Unit, 0), ReconnectPolicy.NextDelay(3, Unit, -5));
        Assert.AreEqual(ReconnectPolicy.NextDelay(3, Unit, 1), ReconnectPolicy.NextDelay(3, Unit, 7));
    }

    [TestMethod]
    public void OnlyAnIdentifiedConnectionThatStayedUpIsStable()
    {
        var at = new DateTimeOffset(2026, 11, 11, 12, 0, 0, TimeSpan.Zero);
        Assert.IsFalse(ReconnectPolicy.WasStable(null, at), "never identified");
        Assert.IsFalse(ReconnectPolicy.WasStable(at, at + TimeSpan.FromSeconds(9.9)), "dropped before it settled");
        Assert.IsTrue(ReconnectPolicy.WasStable(at, at + ReconnectPolicy.StableAfter));
    }
}
