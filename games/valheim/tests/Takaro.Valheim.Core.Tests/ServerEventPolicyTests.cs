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

    [DataTestMethod]
    [DataRow("IceSkates", "Ice Skates")]
    [DataRow("Bjorn_spiritcaller", "Bjorn Spiritcaller")]
    [DataRow("LastBossGate_RuneTile", "Last Boss Gate Rune Tile")]
    [DataRow("TrophyDeerWhite", "Trophy Deer White")]
    [DataRow("Boar", "Boar")]
    [DataRow("TheHive", "The Hive")]
    [DataRow("enemy_wolf", "Enemy Wolf")]
    [DataRow("Pot_Shard_Red", "Pot Shard Red")]
    [DataRow("", "")]
    public void MakesUntranslatedPrefabCodesReadable(string code, string expected)
    {
        Assert.AreEqual(expected, ValheimDisplayName.FromCode(code));
    }

    [TestMethod]
    public void WrittenEventsStayQueuedUntilAPongConfirmsThem()
    {
        var queue = new PendingEventQueue(capacity: 10);
        queue.Enqueue("one");
        queue.Enqueue("two");

        Assert.IsTrue(queue.TryPeekUnsent(out var frame));
        queue.MarkSent(frame);
        Assert.IsTrue(queue.TryPeekUnsent(out frame));
        Assert.AreEqual("two", frame);
        queue.MarkSent(frame);
        Assert.IsFalse(queue.TryPeekUnsent(out _));
        Assert.AreEqual(2, queue.Count, "a write alone is not delivery");

        queue.AddCheckpoint();
        Assert.AreEqual(2, queue.ConfirmOldestCheckpoint());
        Assert.AreEqual(0, queue.Count);
    }

    [TestMethod]
    public void APongOnlyConfirmsFramesWrittenBeforeItsPing()
    {
        var queue = new PendingEventQueue(capacity: 10);
        queue.Enqueue("before");
        queue.TryPeekUnsent(out var frame);
        queue.MarkSent(frame);
        queue.AddCheckpoint();
        queue.Enqueue("after");
        queue.TryPeekUnsent(out frame);
        queue.MarkSent(frame);

        Assert.AreEqual(1, queue.ConfirmOldestCheckpoint());
        Assert.AreEqual(1, queue.Count);
        Assert.AreEqual(0, queue.ConfirmOldestCheckpoint(), "no ping was written after 'after'");
    }

    [TestMethod]
    public void UnconfirmedEventsAreResentAfterAReconnect()
    {
        var queue = new PendingEventQueue(capacity: 10);
        queue.Enqueue("lost in outage");
        queue.TryPeekUnsent(out var frame);
        queue.MarkSent(frame);
        queue.AddCheckpoint();

        Assert.AreEqual(1, queue.ResetForNewConnection());
        Assert.AreEqual(0, queue.OutstandingCheckpoints, "pongs of a dead connection never come");
        Assert.IsTrue(queue.TryPeekUnsent(out frame));
        Assert.AreEqual("lost in outage", frame);
    }

    [TestMethod]
    public void PendingEventsDropTheOldestWhenFull()
    {
        var queue = new PendingEventQueue(capacity: 2);
        queue.Enqueue("one");
        queue.Enqueue("two");
        queue.Enqueue("three");

        Assert.AreEqual(1L, queue.Dropped);
        Assert.IsTrue(queue.TryPeekUnsent(out var head));
        Assert.AreEqual("two", head);
    }
}
