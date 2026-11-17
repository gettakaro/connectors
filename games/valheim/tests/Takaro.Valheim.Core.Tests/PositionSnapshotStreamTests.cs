using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PositionSnapshotStreamTests
{
    [TestMethod]
    public void DelayedDeliveryWaitsForMatchingAckThenTheDeclaredInterval()
    {
        long now = 0;
        var stream = new PositionSnapshotStream(() => now);
        Assert.IsTrue(stream.TryReserve(out var seq));
        now = TimeSpan.FromSeconds(20).Ticks;
        Assert.IsFalse(stream.TryReserve(out _), "A stalled send cannot queue more snapshots.");
        Assert.IsFalse(stream.Acknowledge("gameplay", seq));
        Assert.IsFalse(stream.Acknowledge(stream.StreamId, seq + 1), "A future ack cannot release an unsent snapshot.");
        Assert.IsTrue(stream.Acknowledge(stream.StreamId, seq));
        now += TimeSpan.FromSeconds(PositionSnapshotStream.IntervalSeconds).Ticks - 1;
        Assert.IsFalse(stream.TryReserve(out _));
        now++;
        Assert.IsTrue(stream.TryReserve(out var next));
        Assert.AreEqual(seq + 1, next);
        Assert.IsFalse(stream.Acknowledge(stream.StreamId, seq), "An older ack cannot release the new snapshot.");
    }

    [TestMethod]
    public void MissingAckTimesOutAndANewConnectionHasNoOldSnapshot()
    {
        long now = 0;
        var old = new PositionSnapshotStream(() => now);
        Assert.IsTrue(old.TryReserve(out var seq));
        now = PositionSnapshotStream.AckTimeout.Ticks - 1;
        Assert.IsFalse(old.HasTimedOut);
        now++;
        Assert.IsTrue(old.HasTimedOut);
        var current = new PositionSnapshotStream(() => now);
        Assert.AreNotEqual(old.StreamId, current.StreamId);
        Assert.IsTrue(current.TryReserve(out var fresh));
        Assert.AreEqual(1L, fresh);
        Assert.IsFalse(current.Acknowledge(old.StreamId, seq));
        current.Cancel(fresh);
        now += PositionSnapshotStream.AckTimeout.Ticks;
        Assert.IsFalse(current.HasTimedOut, "A skipped incomplete capture does not await an ack.");
        Assert.IsTrue(current.TryReserve(out _));
    }
}
