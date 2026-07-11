using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class RuntimeArrayActionPolicyTests
{
    [TestMethod]
    public void MissingRuntimeSourceReturnsActionableErrorInsteadOfEmptyArray()
    {
        var result = RuntimeArrayActionPolicy.FromSource<TakaroPlayer>(
            sourceAvailable: false,
            values: null,
            sourceName: "Valheim networking");

        Assert.IsFalse(result.Success);
        Assert.AreEqual("runtime_unavailable", result.ErrorCode);
        StringAssert.Contains(result.Message, "Valheim networking");
        Assert.IsNull(result.Payload);
    }

    [TestMethod]
    public void AvailableRuntimeSourceMayConfirmAnEmptyArray()
    {
        var result = RuntimeArrayActionPolicy.FromSource(
            sourceAvailable: true,
            values: Array.Empty<TakaroPlayer>(),
            sourceName: "Valheim networking");

        Assert.IsTrue(result.Success);
        Assert.IsInstanceOfType<TakaroPlayer[]>(result.Payload);
        Assert.AreEqual(0, ((TakaroPlayer[])result.Payload!).Length);
    }

    [TestMethod]
    public void AvailableRuntimeWithMissingCollectionIsStillUnavailable()
    {
        var result = RuntimeArrayActionPolicy.FromSource<TakaroPlayer>(
            sourceAvailable: true,
            values: null,
            sourceName: "Valheim player list");

        Assert.IsFalse(result.Success);
        Assert.AreEqual("runtime_unavailable", result.ErrorCode);
    }

    [TestMethod]
    public void PlayerLookupConvertsAbsentPlayerToActionableError()
    {
        var players = RuntimeArrayActionPolicy.FromSource(
            true,
            new[] { new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null) },
            "Valheim player list");

        var result = RuntimePlayerActionPolicy.Find(players, "Steam_missing");

        Assert.IsFalse(result.Success);
        Assert.AreEqual("player_not_found", result.ErrorCode);
        StringAssert.Contains(result.Message, "Steam_missing");
    }

    [TestMethod]
    public void PlayerLookupPropagatesUnavailableRuntimeWithoutInventingNotFound()
    {
        var unavailable = RuntimeArrayActionPolicy.FromSource<TakaroPlayer>(false, null, "Valheim networking");

        var result = RuntimePlayerActionPolicy.Find(unavailable, "Steam_1");

        Assert.IsFalse(result.Success);
        Assert.AreEqual("runtime_unavailable", result.ErrorCode);
    }
}
