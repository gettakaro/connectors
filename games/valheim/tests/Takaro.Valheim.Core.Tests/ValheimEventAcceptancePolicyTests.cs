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
    public void RejectsIdentityBearingEventsFromRoutedRpcPayloads(string eventType)
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.RoutedRpcPayload));
    }

    [DataTestMethod]
    [DataRow("chat-message")]
    [DataRow("player-death")]
    [DataRow("entity-killed")]
    public void AcceptsOwnedReportEventsFromClientCompanion(string eventType)
    {
        Assert.IsTrue(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.ClientCompanion));
    }

    [DataTestMethod]
    [DataRow("log")]
    [DataRow("player-connected")]
    [DataRow("player-disconnected")]
    [DataRow("unknown-event")]
    public void RejectsOtherEventsFromClientCompanion(string eventType)
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            eventType,
            ValheimEventObservationSource.ClientCompanion));
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
            ValheimEventObservationSource.RoutedRpcPayload));
    }

    [TestMethod]
    public void RejectsUnsupportedEntityDeathsFromServerCharacterState()
    {
        Assert.IsFalse(ValheimEventAcceptancePolicy.CanEmit(
            "entity-killed",
            ValheimEventObservationSource.ServerCharacterState));
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
