using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PlayerLifecycleEventTrackerTests
{
    [TestMethod]
    public void FirstSnapshotSeedsStateWithoutEmittingEvents()
    {
        var tracker = new PlayerLifecycleEventTracker();

        var events = tracker.Update(new[] { Player("Steam_1", "Odin") }, Now()).ToArray();

        Assert.AreEqual(0, events.Length);
    }

    [TestMethod]
    public void EmitsConnectedEventForNewPlayerAfterInitialSnapshot()
    {
        var tracker = new PlayerLifecycleEventTracker();
        tracker.Update(Array.Empty<TakaroPlayer>(), Now()).ToArray();

        var events = tracker.Update(new[] { Player("Steam_1", "Odin") }, Now()).ToArray();

        Assert.AreEqual(1, events.Length);
        Assert.AreEqual("player-connected", events[0].Type);
        Assert.AreEqual("Steam_1", events[0].Player.GameId);
    }

    [TestMethod]
    public void EmitsDisconnectedEventForMissingPlayer()
    {
        var tracker = new PlayerLifecycleEventTracker();
        tracker.Update(new[] { Player("Steam_1", "Odin") }, Now()).ToArray();

        var events = tracker.Update(Array.Empty<TakaroPlayer>(), Now()).ToArray();

        Assert.AreEqual(1, events.Length);
        Assert.AreEqual("player-disconnected", events[0].Type);
        Assert.AreEqual("Steam_1", events[0].Player.GameId);
    }

    [TestMethod]
    public void DoesNotEmitEventsWhenSamePlayerRemainsOnline()
    {
        var tracker = new PlayerLifecycleEventTracker();
        tracker.Update(new[] { Player("Steam_1", "Odin") }, Now()).ToArray();

        var events = tracker.Update(new[] { Player("Steam_1", "Odin Updated") }, Now()).ToArray();

        Assert.AreEqual(0, events.Length);
    }

    private static DateTimeOffset Now() => new(2026, 6, 21, 8, 0, 0, TimeSpan.Zero);

    private static TakaroPlayer Player(string gameId, string name) =>
        new(
            GameId: gameId,
            Name: name,
            SteamId: null,
            PlatformId: null,
            Ip: null,
            Ping: null);
}
