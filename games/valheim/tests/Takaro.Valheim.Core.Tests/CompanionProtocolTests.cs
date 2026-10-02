using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionProtocolTests
{
    private static readonly JsonSerializerOptions Wire = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    [TestMethod]
    public void ProtocolThreeIsTheOnlySupportedVersionAndTheChannelNameIsStable()
    {
        Assert.AreEqual("TakaroCompanionV1", CompanionProtocol.RpcName);
        Assert.AreEqual(3, CompanionProtocol.CurrentVersion);
        Assert.AreEqual(3, CompanionProtocol.MinimumVersion);
        Assert.AreEqual(2, CompanionProtocol.NegotiationEnvelopeVersion);
        Assert.AreEqual(2, (int)CompanionCapability.Inventory);
    }

    [TestMethod]
    public void OnlyInventoryAndKillVerdictReportsExist()
    {
        foreach (var removed in new[] { "chat", "server-chat", "player-death", "entity-killed", "item-grant" })
        {
            Assert.IsFalse(
                CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(3, removed, "{}"), out _, out var error),
                removed);
            Assert.AreEqual("unknown-message-type", error);
        }
    }

    [TestMethod]
    public void NegotiationMessagesDecodeAtAnyVersionSoAMismatchCanBeAnswered()
    {
        var legacyHello = EnvelopeJson(2, CompanionMessageTypes.Hello, """{"minimumVersion":2,"maximumVersion":2,"capabilities":63}""");
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(legacyHello, out var envelope, out _));
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionHello>(envelope!, out var hello, out _));
        Assert.AreEqual(63, hello!.Capabilities);

        var futureNack = EnvelopeJson(7, CompanionMessageTypes.HelloNack, """{"minimumVersion":7,"maximumVersion":9,"productVersion":"9.0.0"}""");
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(futureNack, out _, out _));
    }

    [TestMethod]
    public void ReportsMustUseExactlyProtocolThree()
    {
        var inventory = """{"stacks":[]}""";
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(3, CompanionMessageTypes.InventorySnapshot, inventory), out _, out _));
        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(2, CompanionMessageTypes.InventorySnapshot, inventory), out _, out var older));
        Assert.AreEqual("unsupported-protocol-version", older);
        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(4, CompanionMessageTypes.KillVerdict, VerdictJson()), out _, out var newer));
        Assert.AreEqual("unsupported-protocol-version", newer);
    }

    [TestMethod]
    public void KillVerdictRoundTripsWithExactlyTheFiveFields()
    {
        var verdict = new CompanionKillVerdict("-123456789:42", "Boar", "$enemy_boar", true, CompanionAttackerKind.LocalPlayer);
        var json = CompanionEnvelopeCodec.EncodeEnvelope(Envelope(CompanionMessageTypes.KillVerdict, verdict));

        using (var document = JsonDocument.Parse(json))
        {
            var names = document.RootElement.GetProperty("payload").EnumerateObject().Select(p => p.Name).ToArray();
            CollectionAssert.AreEquivalent(
                new[] { "creatureZdo", "prefab", "enemyToken", "lastHitByLocalPlayer", "lastHitAttackerKind" },
                names);
        }

        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _));
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionKillVerdict>(envelope!, out var decoded, out _));
        Assert.AreEqual(verdict, decoded);
    }

    [TestMethod]
    [DataRow("""{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"local-player","player":"someone"}""", DisplayName = "extra identity field")]
    [DataRow("""{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true}""", DisplayName = "missing field")]
    [DataRow("""{"creatureZdo":"not-a-zdo","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"local-player"}""", DisplayName = "bad zdo")]
    [DataRow("""{"creatureZdo":"12:-3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"local-player"}""", DisplayName = "negative id")]
    [DataRow("""{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"wolf"}""", DisplayName = "unknown kind")]
    [DataRow("""{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"creature"}""", DisplayName = "flag contradicts kind")]
    [DataRow("""{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":"yes","lastHitAttackerKind":"local-player"}""", DisplayName = "string flag")]
    [DataRow("""{"creatureZdo":"12:3","prefab":" ","enemyToken":"$enemy_boar","lastHitByLocalPlayer":false,"lastHitAttackerKind":"none"}""", DisplayName = "blank prefab")]
    public void MalformedKillVerdictsAreRejected(string payload)
    {
        if (!CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(3, CompanionMessageTypes.KillVerdict, payload), out var envelope, out _))
        {
            return;
        }

        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodePayload<CompanionKillVerdict>(envelope!, out _, out _));
    }

    [TestMethod]
    public void EveryAttackerKindDecodes()
    {
        foreach (var kind in new[] { CompanionAttackerKind.LocalPlayer, CompanionAttackerKind.OtherPlayer, CompanionAttackerKind.Creature, CompanionAttackerKind.None })
        {
            var verdict = new CompanionKillVerdict("1:2", "Greyling", "$enemy_greyling", kind == CompanionAttackerKind.LocalPlayer, kind);
            var json = CompanionEnvelopeCodec.EncodeEnvelope(Envelope(CompanionMessageTypes.KillVerdict, verdict));
            Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _), kind);
            Assert.IsTrue(CompanionEnvelopeCodec.TryDecodePayload<CompanionKillVerdict>(envelope!, out _, out _), kind);
        }
    }

    [TestMethod]
    public void InventoryStacksCannotClaimIdentityOrExceedBounds()
    {
        var extra = """{"stacks":[{"code":"Wood","name":"$item_wood","amount":5,"quality":1,"durability":0,"equipped":false,"slot":0,"owner":"x"}]}""";
        Assert.IsTrue(CompanionEnvelopeCodec.TryDecodeEnvelope(EnvelopeJson(3, CompanionMessageTypes.InventorySnapshot, extra), out var envelope, out _));
        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodePayload<CompanionInventoryReport>(envelope!, out _, out _));

        var tooMany = new CompanionInventoryReport(Enumerable.Range(0, CompanionProtocol.MaximumInventoryStacks + 1)
            .Select(i => new CompanionInventoryStack("Wood", "$item_wood", 1, 1, 0, false, 0))
            .ToArray());
        Assert.ThrowsException<ArgumentException>(() => CompanionEnvelopeCodec.EncodeEnvelope(Envelope(CompanionMessageTypes.InventorySnapshot, tooMany)));
    }

    [TestMethod]
    public void EnvelopeOverTheByteLimitIsRejectedBeforeParsing()
    {
        var json = new string(' ', CompanionProtocol.MaximumEnvelopeUtf8Bytes + 1);
        Assert.IsFalse(CompanionEnvelopeCodec.TryDecodeEnvelope(json, out _, out var error));
        Assert.AreEqual("envelope-too-large", error);
    }

    [TestMethod]
    public void VersionPolicySelectsTheHighestCommonVersionOrNone()
    {
        Assert.IsTrue(CompanionVersionPolicy.TryNegotiate(3, 3, 2, 5, out var selected));
        Assert.AreEqual(3, selected);
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(3, 3, 2, 2, out _));
        Assert.IsFalse(CompanionVersionPolicy.TryNegotiate(3, 3, 4, 4, out _));
        Assert.AreEqual("3", CompanionVersionPolicy.DescribeRange(3, 3));
        Assert.AreEqual("2-4", CompanionVersionPolicy.DescribeRange(2, 4));
    }

    [TestMethod]
    public void KillObservationBecomesAVerdictOnlyWhenTheLocalPlayerIsMarked()
    {
        var marked = new CompanionKillObservation(-77, 9, "Boar", "$enemy_boar", true, CompanionAttackerKind.Creature);
        var verdict = CompanionKillVerdictPolicy.ToVerdict(marked);
        Assert.IsNotNull(verdict);
        Assert.AreEqual("-77:9", verdict.CreatureZdo);
        Assert.IsFalse(verdict.LastHitByLocalPlayer);
        Assert.AreEqual(CompanionAttackerKind.Creature, verdict.LastHitAttackerKind);

        var byPlayer = CompanionKillVerdictPolicy.ToVerdict(new CompanionKillObservation(1, 2, "Boar", "$enemy_boar", true, CompanionAttackerKind.LocalPlayer));
        Assert.IsTrue(byPlayer!.LastHitByLocalPlayer);

        Assert.IsNull(CompanionKillVerdictPolicy.ToVerdict(new CompanionKillObservation(1, 2, "Boar", "$enemy_boar", false, CompanionAttackerKind.LocalPlayer)));
        Assert.IsNull(CompanionKillVerdictPolicy.ToVerdict(new CompanionKillObservation(0, 0, "Boar", "$enemy_boar", true, CompanionAttackerKind.LocalPlayer)));
        Assert.IsNull(CompanionKillVerdictPolicy.ToVerdict(new CompanionKillObservation(1, 2, null, "$enemy_boar", true, CompanionAttackerKind.LocalPlayer)));
        Assert.IsNull(CompanionKillVerdictPolicy.ToVerdict(new CompanionKillObservation(1, 2, "Boar", "$enemy_boar", true, "wolf")));
    }

    [TestMethod]
    public void ClientSendLimiterBoundsBurstsAndRefills()
    {
        var limiter = new CompanionSendLimiter(capacity: 4, refillPerSecond: 2);
        for (var i = 0; i < 4; i++)
        {
            Assert.IsTrue(limiter.TryConsume(TimeSpan.FromSeconds(10)));
        }

        Assert.IsFalse(limiter.TryConsume(TimeSpan.FromSeconds(10)));
        Assert.IsTrue(limiter.TryConsume(TimeSpan.FromSeconds(10.5)));
        Assert.IsFalse(limiter.TryConsume(TimeSpan.FromSeconds(10.5)));
    }

    internal static CompanionEnvelope Envelope<T>(string type, T payload, int version = 3, string nonce = "nonce", long sequence = 2) =>
        new(version, nonce, sequence, $"{type}-{sequence}", type, JsonSerializer.SerializeToElement(payload, Wire));

    internal static string EnvelopeJson(int version, string type, string payload, string nonce = "nonce", long sequence = 2) =>
        $$"""{"protocolVersion":{{version}},"sessionNonce":"{{nonce}}","sequence":{{sequence}},"messageId":"m-{{sequence}}","type":"{{type}}","payload":{{payload}}}""";

    private static string VerdictJson() =>
        """{"creatureZdo":"12:3","prefab":"Boar","enemyToken":"$enemy_boar","lastHitByLocalPlayer":true,"lastHitAttackerKind":"local-player"}""";
}
