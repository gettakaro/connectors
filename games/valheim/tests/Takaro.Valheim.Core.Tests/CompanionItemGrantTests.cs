using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

/// <summary>
/// Covers the item-grant accounting and its protocol contract.
///
/// The accounting exists because Valheim's <c>Inventory.AddItem</c> fills partially: it
/// deposits stackable units one at a time and, when it runs out of room, reports failure
/// while keeping everything it already added. Its return value therefore cannot say how
/// much arrived, so the companion measures the inventory before and after instead. These
/// tests pin that measurement, since an error here either destroys a player's items or
/// duplicates them.
/// </summary>
[TestClass]
public class CompanionItemGrantTests
{
    [TestMethod]
    public void EverythingFittingIsDeliveredWithNothingDropped()
    {
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 5, countBefore: 0, countAfter: 5);

        Assert.IsTrue(outcome.Resolved);
        Assert.AreEqual(5, outcome.Delivered);
        Assert.AreEqual(0, outcome.Dropped);
    }

    [TestMethod]
    public void MergingIntoAnExistingStackCountsOnlyTheAddedUnits()
    {
        // The player already holds 40; a grant of 5 must be measured as 5, not as 45.
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 5, countBefore: 40, countAfter: 45);

        Assert.AreEqual(5, outcome.Delivered);
        Assert.AreEqual(0, outcome.Dropped);
    }

    [TestMethod]
    public void PartialFillReportsTheShortfallSoItCanBeDropped()
    {
        // AddItem got 60 of 100 in before running out of room and kept them.
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 100, countBefore: 0, countAfter: 60);

        Assert.AreEqual(60, outcome.Delivered);
        Assert.AreEqual(40, outcome.Dropped);
    }

    [TestMethod]
    public void AFullInventoryDropsTheWholeGrant()
    {
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 7, countBefore: 12, countAfter: 12);

        Assert.AreEqual(0, outcome.Delivered);
        Assert.AreEqual(7, outcome.Dropped);
    }

    [TestMethod]
    public void DeliveredAndDroppedAlwaysAccountForTheWholeGrant()
    {
        // No observed delta may cause units to vanish or multiply, including the nonsense
        // ones a concurrent inventory mutation could produce.
        foreach (var (before, after) in new[] { (0, 0), (0, 3), (0, 9), (5, 4), (5, 99), (7, 7) })
        {
            var outcome = CompanionItemGrantMath.FromCountDelta(requested: 9, countBefore: before, countAfter: after);

            Assert.AreEqual(
                9,
                outcome.Delivered + outcome.Dropped,
                $"delta {before}->{after} did not account for the full grant");
            Assert.IsTrue(outcome.Delivered >= 0 && outcome.Dropped >= 0);
        }
    }

    [TestMethod]
    public void ADeltaLargerThanTheGrantNeverInventsDeliveredUnits()
    {
        // Another source added items between the two reads. Crediting the surplus to this
        // grant would let a player claim more than was granted.
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 3, countBefore: 0, countAfter: 50);

        Assert.AreEqual(3, outcome.Delivered);
        Assert.AreEqual(0, outcome.Dropped);
    }

    [TestMethod]
    public void ANegativeDeltaIsTreatedAsNothingDelivered()
    {
        // The player dropped or consumed items mid-grant; the safe reading is that none of
        // the grant landed, so all of it is re-dropped rather than silently lost.
        var outcome = CompanionItemGrantMath.FromCountDelta(requested: 4, countBefore: 10, countAfter: 6);

        Assert.AreEqual(0, outcome.Delivered);
        Assert.AreEqual(4, outcome.Dropped);
    }

    [TestMethod]
    public void UnresolvedGrantsDeliverNothingAndDropNothing()
    {
        // An unknown item code resolves nothing, so the companion must not pretend to have
        // dropped items it never created.
        var outcome = CompanionItemGrantOutcome.Unresolved(6);

        Assert.IsFalse(outcome.Resolved);
        Assert.AreEqual(0, outcome.Delivered);
        Assert.AreEqual(0, outcome.Dropped);
        Assert.AreEqual(string.Empty, CompanionItemGrantMath.DescribeOutcome(outcome, "Wood"));
    }

    [TestMethod]
    public void PlayerNoticeDistinguishesTheThreeOutcomes()
    {
        var delivered = CompanionItemGrantMath.DescribeOutcome(
            CompanionItemGrantMath.FromCountDelta(5, 0, 5),
            "Wood");
        var split = CompanionItemGrantMath.DescribeOutcome(
            CompanionItemGrantMath.FromCountDelta(10, 0, 4),
            "Wood");
        var dropped = CompanionItemGrantMath.DescribeOutcome(
            CompanionItemGrantMath.FromCountDelta(10, 0, 0),
            "Wood");

        StringAssert.Contains(delivered, "Received 5x Wood");
        Assert.IsFalse(delivered.Contains("dropped"), "a clean delivery must not mention dropping");

        StringAssert.Contains(split, "Received 4x Wood");
        StringAssert.Contains(split, "6 dropped at your feet");

        StringAssert.Contains(dropped, "inventory was full");
        StringAssert.Contains(dropped, "10x Wood");
    }

    [TestMethod]
    public void ItemGrantRoundTripsAcrossTheWire()
    {
        var envelope = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "nonce",
            2,
            "item-grant-2",
            CompanionMessageTypes.ItemGrant,
            ToJsonElement("""{"code":"Wood","amount":5,"quality":1}"""));

        var encoded = CompanionEnvelopeCodec.EncodeEnvelope(envelope);

        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(encoded, out var decoded, out _));
        Assert.IsNotNull(decoded);
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionItemGrant>(
            decoded!,
            out var grant,
            out _));
        Assert.IsNotNull(grant);
        Assert.AreEqual("Wood", grant!.Code);
        Assert.AreEqual(5, grant.Amount);
        Assert.AreEqual(1, grant.Quality);
    }

    [DataTestMethod]
    [DataRow("""{"code":"Wood","amount":0,"quality":1}""", "zero amount")]
    [DataRow("""{"code":"Wood","amount":-1,"quality":1}""", "negative amount")]
    [DataRow("""{"code":"","amount":5,"quality":1}""", "blank code")]
    [DataRow("""{"code":"Wood","amount":5,"quality":0}""", "zero quality")]
    [DataRow("""{"code":"Wood","amount":5}""", "missing quality")]
    [DataRow("""{"code":"Wood","amount":5,"quality":1,"playerId":"claimed"}""", "claimed identity")]
    public void ItemGrantRejectsInvalidPayloads(string payloadJson, string caseName)
    {
        // A grant that fails validation must be refused outright rather than applied with a
        // guessed amount, since the companion mutates a real inventory from it.
        using var document = JsonDocument.Parse(payloadJson);
        var envelope = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "nonce",
            2,
            "item-grant-2",
            CompanionMessageTypes.ItemGrant,
            document.RootElement.Clone());

        Assert.IsFalse(
            CompanionEnvelopeCodec.TryDecodePayload<CompanionItemGrant>(envelope, out _, out var error),
            caseName);
        Assert.IsNotNull(error, caseName);
    }

    [TestMethod]
    public void ItemGrantIsPartOfTheNegotiatedCapabilitySet()
    {
        // The capability bit gates the server's send and the client's accept. If it were not
        // a known capability, a hello advertising it would be rejected as an invalid payload.
        Assert.AreEqual(32, (int)CompanionCapability.ItemGrant);

        var hello = new CompanionEnvelope(
            CompanionProtocol.CurrentVersion,
            "nonce",
            1,
            "hello",
            CompanionMessageTypes.Hello,
            ToJsonElement(
                $$"""{"minimumVersion":{{CompanionProtocol.MinimumVersion}},"maximumVersion":{{CompanionProtocol.CurrentVersion}},"capabilities":33}"""));

        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHello>(hello, out var decoded, out _));
        Assert.IsNotNull(decoded);
        Assert.IsTrue(decoded!.Capabilities.HasFlag(CompanionCapability.ItemGrant));
    }

    private static JsonElement ToJsonElement(string json)
    {
        using var document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }
}
