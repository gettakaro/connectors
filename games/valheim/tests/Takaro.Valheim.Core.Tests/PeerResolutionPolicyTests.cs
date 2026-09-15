using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PeerResolutionPolicyTests
{
    [TestMethod]
    public void AssociationPrefersStableIdentityOverEarlierDuplicateName()
    {
        var wrongSource = new object();
        var expectedSource = new object();
        var candidates = new[]
        {
            Candidate(wrongSource, 10, true, "wrong-character", "Steam_wrong", "wrong", "Shared Name"),
            Candidate(expectedSource, 20, true, "expected-character", "Steam_expected", "expected", "Shared Name")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: null,
            stableIdentifiers: new[] { "expected" },
            names: new[] { "Shared Name" },
            out var resolved,
            out var ambiguous);

        Assert.IsTrue(found);
        Assert.IsFalse(ambiguous);
        Assert.AreSame(expectedSource, resolved?.Source);
        Assert.AreEqual("expected", resolved?.Player.GameId);
    }

    [TestMethod]
    public void AssociationPrefersCharacterIdOverEarlierDuplicateName()
    {
        var wrongSource = new object();
        var expectedSource = new object();
        var candidates = new[]
        {
            Candidate(wrongSource, 10, true, "wrong-character", "Steam_wrong", "wrong", "Shared Name"),
            Candidate(expectedSource, 20, true, "expected-character", "Steam_expected", "expected", "Shared Name")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: "expected-character",
            stableIdentifiers: new[] { "wrong" },
            names: new[] { "Shared Name" },
            out var resolved,
            out var ambiguous);

        Assert.IsTrue(found);
        Assert.IsFalse(ambiguous);
        Assert.AreSame(expectedSource, resolved?.Source);
    }

    [TestMethod]
    public void AssociationMatchesStableIdentityCaseInsensitively()
    {
        var expectedSource = new object();
        var candidates = new[]
        {
            Candidate(expectedSource, 20, true, "character", "Steam_expected", "expected", "Expected")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: null,
            stableIdentifiers: new[] { "EXPECTED" },
            names: Array.Empty<string?>(),
            out var resolved,
            out var ambiguous);

        Assert.IsTrue(found);
        Assert.IsFalse(ambiguous);
        Assert.AreSame(expectedSource, resolved?.Source);
    }

    [TestMethod]
    public void AssociationRejectsDuplicateCharacterIdsWithoutWeakeningMatch()
    {
        var candidates = new[]
        {
            Candidate(new object(), 10, true, "duplicate", "Steam_one", "one", "First"),
            Candidate(new object(), 20, true, "duplicate", "Steam_two", "two", "Second")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: "duplicate",
            stableIdentifiers: new[] { "one" },
            names: new[] { "First" },
            out var resolved,
            out var ambiguous);

        Assert.IsFalse(found);
        Assert.IsTrue(ambiguous);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void AssociationRejectsDuplicateStableMatchesWithoutUsingUniqueName()
    {
        var candidates = new[]
        {
            Candidate(new object(), 10, true, "one", "shared-stable", "one", "Unique Name"),
            Candidate(new object(), 20, true, "two", "other", "shared-stable", "Other Name")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: null,
            stableIdentifiers: new[] { "shared-stable" },
            names: new[] { "Unique Name" },
            out var resolved,
            out var ambiguous);

        Assert.IsFalse(found);
        Assert.IsTrue(ambiguous);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void AssociationRejectsDuplicateNames()
    {
        var candidates = new[]
        {
            Candidate(new object(), 10, true, "one", "Steam_one", "one", "Shared"),
            Candidate(new object(), 20, true, "two", "Steam_two", "two", "Shared")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: null,
            stableIdentifiers: Array.Empty<string?>(),
            names: new[] { "Shared" },
            out var resolved,
            out var ambiguous);

        Assert.IsFalse(found);
        Assert.IsTrue(ambiguous);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void AssociationAllowsOneReadyNameFallback()
    {
        var expectedSource = new object();
        var candidates = new[]
        {
            Candidate(new object(), 10, true, "one", "Steam_one", "one", "Other"),
            Candidate(expectedSource, 20, true, "two", "Steam_two", "two", "Unique")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: null,
            stableIdentifiers: Array.Empty<string?>(),
            names: new[] { "Unique" },
            out var resolved,
            out var ambiguous);

        Assert.IsTrue(found);
        Assert.IsFalse(ambiguous);
        Assert.AreSame(expectedSource, resolved?.Source);
    }

    [TestMethod]
    public void AssociationDoesNotSelectAnUnreadyPeer()
    {
        var candidates = new[]
        {
            Candidate(new object(), 10, false, "target", "Steam_target", "target", "Target")
        };

        var found = PeerResolutionPolicy.TryAssociate(
            candidates,
            characterId: "target",
            stableIdentifiers: new[] { "target" },
            names: new[] { "Target" },
            out var resolved,
            out var ambiguous);

        Assert.IsFalse(found);
        Assert.IsFalse(ambiguous);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void AssociationHandlesNullCandidatesAsMissingNetworking()
    {
        var found = PeerResolutionPolicy.TryAssociate<object>(
            candidates: null,
            characterId: "target",
            stableIdentifiers: new[] { "target" },
            names: new[] { "Target" },
            out var resolved,
            out var ambiguous);

        Assert.IsFalse(found);
        Assert.IsFalse(ambiguous);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void ReadySenderReturnsTheExactMappedCandidate()
    {
        var expectedSource = new object();
        var candidates = new[]
        {
            Candidate(new object(), 10, true, "one", "Steam_one", "one", "First"),
            Candidate(expectedSource, 20, true, "two", "Steam_two", "two", "Expected")
        };

        var found = PeerResolutionPolicy.TryResolveReadySender(candidates, 20, out var resolved);

        Assert.IsTrue(found);
        Assert.AreSame(expectedSource, resolved?.Source);
        Assert.AreEqual("Expected", resolved?.Player.Name);
    }

    [TestMethod]
    public void ReadySenderRejectsDuplicateUidEvenWhenOnlyOneIsReady()
    {
        var candidates = new[]
        {
            Candidate(new object(), 20, true, "one", "Steam_one", "one", "First"),
            Candidate(new object(), 20, false, "two", "Steam_two", "two", "Second")
        };

        var found = PeerResolutionPolicy.TryResolveReadySender(candidates, 20, out var resolved);

        Assert.IsFalse(found);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void ReadySenderRejectsOneUnreadyPeer()
    {
        var candidates = new[]
        {
            Candidate(new object(), 20, false, "one", "Steam_one", "one", "First")
        };

        var found = PeerResolutionPolicy.TryResolveReadySender(candidates, 20, out var resolved);

        Assert.IsFalse(found);
        Assert.IsNull(resolved);
    }

    [TestMethod]
    public void ReadySenderHandlesNullCandidatesAsMissingNetworking()
    {
        var found = PeerResolutionPolicy.TryResolveReadySender<object>(null, 20, out var resolved);

        Assert.IsFalse(found);
        Assert.IsNull(resolved);
    }

    private static PeerResolutionCandidate<object> Candidate(
        object source,
        long peerUid,
        bool isReady,
        string? characterId,
        string? hostName,
        string gameId,
        string name) =>
        new(
            source,
            peerUid,
            isReady,
            characterId,
            hostName,
            new TakaroPlayer(gameId, name, null, $"valheim:{gameId}", null, null));
}
