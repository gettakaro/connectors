using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ServerEventPolicyTests
{
    private static readonly DateTimeOffset Now = new(2026, 10, 1, 20, 0, 0, TimeSpan.Zero);

    private static RoutedPacketOrigin Origin(long arrival = 42, long claimed = 42, string? character = "42:7", string? target = "42:7") =>
        new(arrival, claimed, character, target);

    [TestMethod]
    public void BindsWhenClaimedSenderIsTheArrivalPeer()
    {
        Assert.AreEqual(RoutedBindingResult.Bound, RoutedPacketBindingPolicy.Evaluate(Origin(), requireOwnCharacter: true));
    }

    [TestMethod]
    public void RejectsASpoofedSenderId()
    {
        Assert.AreEqual(
            RoutedBindingResult.SenderMismatch,
            RoutedPacketBindingPolicy.Evaluate(Origin(arrival: 42, claimed: 99), requireOwnCharacter: false));
    }

    [TestMethod]
    public void RejectsPacketsWithNoKnownArrivalPeer()
    {
        Assert.AreEqual(
            RoutedBindingResult.NoArrivalPeer,
            RoutedPacketBindingPolicy.Evaluate(Origin(arrival: 0, claimed: 0), requireOwnCharacter: false));
    }

    [TestMethod]
    public void RejectsCharacterRpcsAimedAtSomeoneElsesCharacter()
    {
        Assert.AreEqual(
            RoutedBindingResult.CharacterMismatch,
            RoutedPacketBindingPolicy.Evaluate(Origin(target: "77:1"), requireOwnCharacter: true));
        Assert.AreEqual(
            RoutedBindingResult.CharacterMismatch,
            RoutedPacketBindingPolicy.Evaluate(Origin(character: null), requireOwnCharacter: true));
    }

    [TestMethod]
    public void EmitsABoundChatLineOnceAcrossItsCopies()
    {
        var policy = new ServerChatRelayPolicy();

        var first = policy.Evaluate(Origin(), characterScoped: true, (int)ValheimTalkerType.Normal, "  hello  ", Now);
        var copy = policy.Evaluate(Origin(), characterScoped: true, (int)ValheimTalkerType.Normal, "hello", Now.AddMilliseconds(5));

        Assert.IsTrue(first.Emit);
        Assert.AreEqual("hello", first.Text);
        Assert.IsFalse(copy.Emit);
        Assert.AreEqual("duplicate-copy", copy.Reason);
    }

    [TestMethod]
    public void EmitsTheSameLineAgainAfterTheWindow()
    {
        var policy = new ServerChatRelayPolicy();
        policy.Evaluate(Origin(), true, (int)ValheimTalkerType.Normal, "again", Now);

        var later = policy.Evaluate(Origin(), true, (int)ValheimTalkerType.Normal, "again", Now + ServerChatRelayPolicy.DuplicateWindow);

        Assert.IsTrue(later.Emit);
    }

    [TestMethod]
    public void KeepsDifferentSendersApart()
    {
        var policy = new ServerChatRelayPolicy();
        Assert.IsTrue(policy.Evaluate(Origin(), true, 1, "hi", Now).Emit);
        Assert.IsTrue(policy.Evaluate(Origin(arrival: 7, claimed: 7, character: "7:1", target: "7:1"), true, 1, "hi", Now).Emit);
    }

    [TestMethod]
    public void NeverEmitsASpoofedChatLine()
    {
        var policy = new ServerChatRelayPolicy();
        var decision = policy.Evaluate(Origin(arrival: 42, claimed: 99), false, (int)ValheimTalkerType.Shout, "pretend", Now);

        Assert.IsFalse(decision.Emit);
        StringAssert.StartsWith(decision.Reason, "unbound");
    }

    [DataTestMethod]
    [DataRow((int)ValheimTalkerType.Ping, "x", "not-a-chat-line")]
    [DataRow(-1, "x", "not-a-chat-line")]
    [DataRow((int)ValheimTalkerType.Normal, "   ", "empty")]
    [DataRow((int)ValheimTalkerType.Normal, null, "empty")]
    public void SkipsPingsAndEmptyLines(int talkerType, string? text, string reason)
    {
        var decision = new ServerChatRelayPolicy().Evaluate(Origin(), true, talkerType, text, Now);

        Assert.IsFalse(decision.Emit);
        Assert.AreEqual(reason, decision.Reason);
    }

    [TestMethod]
    public void RejectsOverlongLines()
    {
        var text = new string('a', ServerChatRelayPolicy.MaximumMessageCharacters + 1);

        Assert.AreEqual("too-long", new ServerChatRelayPolicy().Evaluate(Origin(), true, 1, text, Now).Reason);
    }

    [TestMethod]
    public void OnceWithinWindowCollapsesRepeats()
    {
        var once = new OnceWithinWindow(TimeSpan.FromSeconds(5));

        Assert.IsTrue(once.TryAccept("a", Now));
        Assert.IsFalse(once.TryAccept("a", Now.AddSeconds(4)));
        Assert.IsTrue(once.TryAccept("b", Now.AddSeconds(4)));
        Assert.IsTrue(once.TryAccept("a", Now.AddSeconds(5)));
    }

    [TestMethod]
    public void KillGoesToTheDestroyerWhenItHitTheCreature()
    {
        var result = KillAttributionPolicy.Choose(
            new[] { new KillCandidate(1, true), new KillCandidate(2, true) },
            destroyingPeerUid: 2);

        Assert.AreEqual(2L, result.PeerUid);
        Assert.AreEqual("destroyer-hit", result.Reason);
    }

    [TestMethod]
    public void KillGoesToTheOnlyHitterWhenAnotherGameDestroyedIt()
    {
        var result = KillAttributionPolicy.Choose(
            new[] { new KillCandidate(1, true), new KillCandidate(2, false) },
            destroyingPeerUid: 2);

        Assert.AreEqual(1L, result.PeerUid);
        Assert.AreEqual("single-hitter", result.Reason);
    }

    [TestMethod]
    public void KillIsNotAttributedWithoutAPlayerHit()
    {
        var result = KillAttributionPolicy.Choose(new[] { new KillCandidate(1, false) }, destroyingPeerUid: 1);

        Assert.IsNull(result.PeerUid);
        Assert.AreEqual("no-player-hit", result.Reason);
    }

    [TestMethod]
    public void KillIsNotGuessedBetweenSeveralHitters()
    {
        var result = KillAttributionPolicy.Choose(
            new[] { new KillCandidate(1, true), new KillCandidate(2, true) },
            destroyingPeerUid: 3);

        Assert.IsNull(result.PeerUid);
        Assert.AreEqual("ambiguous-hitters", result.Reason);
    }

    [DataTestMethod]
    [DataRow((int)ValheimKillModifier.Unarmed, "Iron Sword", null, "Unarmed")]
    [DataRow((int)ValheimKillModifier.Melee, "Iron Sword", "Wood Shield", "Iron Sword")]
    [DataRow((int)ValheimKillModifier.Ranged, null, "Crude Bow", "Crude Bow")]
    [DataRow((int)ValheimKillModifier.Ranged, "Iron Sword", "Crude Bow", "Crude Bow")]
    [DataRow((int)ValheimKillModifier.Melee, null, null, "Melee")]
    [DataRow((int)ValheimKillModifier.Magic, null, null, "Magic")]
    [DataRow((int)ValheimKillModifier.CountNone, null, null, "Unarmed")]
    [DataRow((int)ValheimKillModifier.MixedAndTotal, "Club", null, "Club")]
    public void NamesTheKillWeapon(int modifier, string? right, string? left, string expected)
    {
        Assert.AreEqual(expected, KillWeaponPolicy.Describe(modifier, right, left));
    }

    [DataTestMethod]
    [DataRow("$item_tin", "Tin", "Tin")]
    [DataRow("Crude Bow", "Bow", "Crude Bow")]
    [DataRow(null, "Bow", "Bow")]
    [DataRow("  ", "Bow", "Bow")]
    public void FallsBackToTheCodeForUntranslatedTokens(string? token, string fallback, string expected)
    {
        Assert.AreEqual(expected, ValheimDisplayName.FromToken(token, fallback));
    }

    [TestMethod]
    public void PendingEventsSurviveUntilAcknowledged()
    {
        var queue = new PendingEventQueue(capacity: 2);
        queue.Enqueue("one");
        queue.Enqueue("two");

        Assert.IsTrue(queue.TryPeek(out var head));
        Assert.AreEqual("one", head);
        Assert.AreEqual(2, queue.Count, "peeking must not remove: a failed send keeps the event");

        queue.Acknowledge(head);
        Assert.IsTrue(queue.TryPeek(out head));
        Assert.AreEqual("two", head);
    }

    [TestMethod]
    public void PendingEventsDropTheOldestWhenFull()
    {
        var queue = new PendingEventQueue(capacity: 2);
        queue.Enqueue("one");
        queue.Enqueue("two");
        queue.Enqueue("three");

        Assert.AreEqual(1L, queue.Dropped);
        Assert.IsTrue(queue.TryPeek(out var head));
        Assert.AreEqual("two", head);
    }

    [TestMethod]
    public void AcknowledgingAStaleFrameDoesNotDropTheNewHead()
    {
        var queue = new PendingEventQueue(capacity: 1);
        queue.Enqueue("old");
        queue.TryPeek(out var sent);
        queue.Enqueue("new");

        queue.Acknowledge(sent);

        Assert.IsTrue(queue.TryPeek(out var head));
        Assert.AreEqual("new", head);
    }

    [DataTestMethod]
    [DataRow("swing_longsword2", "swing_longsword")]
    [DataRow("unarmed_attack0", "unarmed_attack")]
    [DataRow("bow_fire", "bow_fire")]
    [DataRow("  ", "")]
    [DataRow(null, "")]
    public void StripsTheComboStepFromAttackTriggers(string? trigger, string expected)
    {
        Assert.AreEqual(expected, InferredKillPolicy.AttackAnimationBase(trigger));
    }

    [TestMethod]
    public void SingleBlowKillsNeedARecentAttackByThatPlayer()
    {
        Assert.IsTrue(InferredKillPolicy.AttackedJustBefore(Now.AddSeconds(-1), Now));
        Assert.IsTrue(InferredKillPolicy.AttackedJustBefore(Now.AddMilliseconds(300), Now), "destroy and trigger can arrive in either order");
        Assert.IsFalse(InferredKillPolicy.AttackedJustBefore(Now.AddSeconds(-5), Now));
        Assert.IsFalse(InferredKillPolicy.AttackedJustBefore(Now.AddSeconds(2), Now));
        Assert.IsFalse(InferredKillPolicy.AttackedJustBefore(null, Now));
    }

    [DataTestMethod]
    [DataRow(0f, true)]
    [DataRow(20f, true)]
    [DataRow(20.5f, false)]
    [DataRow(float.NaN, false)]
    [DataRow(-1f, false)]
    public void SingleBlowKillsNeedTheKillerNearby(float distance, bool expected)
    {
        Assert.AreEqual(expected, InferredKillPolicy.KillerCloseEnough(distance));
    }
}
