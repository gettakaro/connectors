using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ValheimEventAcceptancePolicyTests
{
    [DataTestMethod]
    [DataRow("chat-message")]
    [DataRow("player-death")]
    [DataRow("entity-killed")]
    [DataRow("player-connected")]
    [DataRow("player-disconnected")]
    [DataRow("log")]
    public void RejectsEveryEventFromUnboundRoutedRpcs(string eventType)
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.UnboundRoutedRpc));
    }

    [DataTestMethod]
    [DataRow("chat-message")]
    [DataRow("player-death")]
    public void AcceptsChatAndDeathOnlyFromPeerBoundRoutedRpcs(string eventType)
    {
        Assert.IsTrue(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.PeerBoundRoutedRpc));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.GameKillReport));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.ServerPlayerSnapshot));
    }

    [TestMethod]
    public void AcceptsEntityKilledOnlyFromGameKillReports()
    {
        Assert.IsTrue(ValheimEventAcceptancePolicy.CanEmit(
            "entity-killed",
            ValheimEventObservationSource.GameKillReport));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            "entity-killed",
            ValheimEventObservationSource.PeerBoundRoutedRpc));
    }

    [DataTestMethod]
    [DataRow("log")]
    [DataRow("player-connected")]
    [DataRow("player-disconnected")]
    [DataRow("unknown-event")]
    public void RejectsOtherEventsFromPeerBoundRoutedRpcs(string eventType)
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.PeerBoundRoutedRpc));
    }

    [DataTestMethod]
    [DataRow("player-connected")]
    [DataRow("player-disconnected")]
    public void AcceptsLifecycleEventsOnlyFromServerPlayerSnapshots(string eventType)
    {
        Assert.IsTrue(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.ServerPlayerSnapshot));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.PeerBoundRoutedRpc));
    }

    [TestMethod]
    public void AcceptsLogEventsOnlyFromTheConnector()
    {
        Assert.IsTrue(ValheimEventAcceptancePolicy.CanEmit(
            "log",
            ValheimEventObservationSource.Connector));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            "log",
            ValheimEventObservationSource.ServerPlayerSnapshot));
    }

    [TestMethod]
    public void ObservationSourcesAreExactlyTheServerSideSet()
    {
        CollectionAssert.AreEquivalent(
            new[] { "Connector", "ServerPlayerSnapshot", "PeerBoundRoutedRpc", "GameKillReport", "UnboundRoutedRpc" },
            Enum.GetNames(typeof(ValheimEventObservationSource)));
    }

    [TestMethod]
    public void RejectsUnknownEventAndSourceCombinations()
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            "chat-message",
            ValheimEventObservationSource.ServerPlayerSnapshot));
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            "unknown-event",
            ValheimEventObservationSource.Connector));
    }
}
