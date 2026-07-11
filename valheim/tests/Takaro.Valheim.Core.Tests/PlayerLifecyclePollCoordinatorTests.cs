using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PlayerLifecyclePollCoordinatorTests
{
    [TestMethod]
    public void TransientLocationFailureDoesNotBecomeADisconnectAtThePollerHandoff()
    {
        var coordinator = new PlayerLifecyclePollCoordinator();
        var player = Player("Steam_1", "Odin");

        Assert.AreEqual(
            0,
            coordinator.Update(
                Array.Empty<TakaroPlayer>(),
                Array.Empty<string>(),
                Now()).Count);

        var connected = coordinator.Update(
            new[] { player },
            new[] { player.GameId },
            Now().AddSeconds(5));
        var transientFailure = coordinator.Update(
            new[] { player },
            Array.Empty<string>(),
            Now().AddSeconds(10));
        var disconnected = coordinator.Update(
            Array.Empty<TakaroPlayer>(),
            Array.Empty<string>(),
            Now().AddSeconds(15));

        Assert.AreEqual("player-connected", AssertSingle(connected).Type);
        Assert.AreEqual(0, transientFailure.Count);
        Assert.AreEqual("player-disconnected", AssertSingle(disconnected).Type);
    }

    private static TakaroPlayerLifecycleEvent AssertSingle(
        IReadOnlyList<TakaroPlayerLifecycleEvent> events)
    {
        Assert.AreEqual(1, events.Count);
        return events[0];
    }

    private static DateTimeOffset Now() =>
        new(2026, 7, 10, 18, 30, 0, TimeSpan.Zero);

    private static TakaroPlayer Player(string gameId, string name) =>
        new(
            GameId: gameId,
            Name: name,
            SteamId: null,
            PlatformId: null,
            Ip: null,
            Ping: null);
}
