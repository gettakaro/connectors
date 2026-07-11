using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PlayerLifecyclePresenceFilterTests
{
    [TestMethod]
    public void TransientLocationFailureKeepsAnAdmittedOnlinePlayerPresent()
    {
        var filter = new PlayerLifecyclePresenceFilter();
        var player = Player("Steam_1", "Odin");
        filter.SelectTrackable(new[] { player }, new[] { player.GameId });

        var trackable = filter.SelectTrackable(new[] { player }, Array.Empty<string>());

        CollectionAssert.AreEqual(new[] { player }, trackable.ToArray());
    }

    [TestMethod]
    public void PlayerIsNotAdmittedUntilServerOwnedLocationHasBeenObserved()
    {
        var filter = new PlayerLifecyclePresenceFilter();
        var player = Player("Steam_1", "Odin");

        var unavailable = filter.SelectTrackable(new[] { player }, Array.Empty<string>());
        var observed = filter.SelectTrackable(new[] { player }, new[] { player.GameId });

        Assert.AreEqual(0, unavailable.Count);
        CollectionAssert.AreEqual(new[] { player }, observed.ToArray());
    }

    [TestMethod]
    public void ActualAbsenceRevokesAdmissionBeforeARejoin()
    {
        var filter = new PlayerLifecyclePresenceFilter();
        var player = Player("Steam_1", "Odin");
        filter.SelectTrackable(new[] { player }, new[] { player.GameId });

        var absent = filter.SelectTrackable(Array.Empty<TakaroPlayer>(), Array.Empty<string>());
        var rejoinedWithoutLocation = filter.SelectTrackable(new[] { player }, Array.Empty<string>());

        Assert.AreEqual(0, absent.Count);
        Assert.AreEqual(0, rejoinedWithoutLocation.Count);
    }

    private static TakaroPlayer Player(string gameId, string name) =>
        new(
            GameId: gameId,
            Name: name,
            SteamId: null,
            PlatformId: null,
            Ip: null,
            Ping: null);
}
