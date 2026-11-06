using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

// Research probe for native Takaro commands (gettakaro/connectors#200).
[TestClass]
public sealed class CommandProbeTests
{
    private const long PeerA = 1001;
    private const long PeerB = 2002;
    private const string NonceA = "nonce-a";
    private const string NonceA2 = "nonce-a2";
    private const string NonceB = "nonce-b";
    private static readonly DateTimeOffset Now = DateTimeOffset.Parse("2026-10-22T10:00:00+00:00");
    private static readonly TakaroPlayer PlayerA = new("Steam_76561190000000001", "Alice", null, "Steam_76561190000000001", null, null);
    private static readonly TakaroPlayer PlayerB = new("Steam_76561190000000002", "Bob", null, "Steam_76561190000000002", null, null);

    [TestMethod]
    public void ProbeEnvelopesRoundTripAndRejectOversizedOrIdentityCarryingPayloads()
    {
        Assert.IsTrue(Decode<CommandProbeInvocation>(Invoke(NonceA, 2, "abc123", "ping", "a", "b"), out var invocation));
        Assert.AreEqual("ping", invocation!.Command);
        CollectionAssert.AreEqual(new[] { "a", "b" }, invocation.Arguments.ToArray());

        Assert.IsTrue(Decode<CommandProbeCatalog>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeCatalog, """{"revision":2,"commands":["ping","balance"]}"""), out _));
        Assert.IsTrue(Decode<CommandProbeReceipt>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeReceipt, """{"invocationId":"abc123","status":"submitted"}"""), out _));

        // No room for a client-asserted identity, role or permission.
        Assert.IsFalse(Decode<CommandProbeInvocation>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeInvoke, """{"invocationId":"abc","command":"ping","arguments":[],"player":"Steam_1"}"""), out _));
        Assert.IsFalse(Decode<CommandProbeInvocation>(Invoke(NonceA, 2, "abc", "ping", Enumerable.Repeat("x", CommandProbeProtocol.MaximumArguments + 1).ToArray()), out _));
        Assert.IsFalse(Decode<CommandProbeInvocation>(Invoke(NonceA, 2, "abc", "ping", new string('x', CommandProbeProtocol.MaximumArgumentCharacters + 1)), out _));
        Assert.IsFalse(Decode<CommandProbeInvocation>(Invoke(NonceA, 2, new string('a', 33), "ping"), out _));
        Assert.IsFalse(Decode<CommandProbeInvocation>(Invoke(NonceA, 2, "abc", "Ping"), out _));
        Assert.IsFalse(Decode<CommandProbeReceipt>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeReceipt, """{"invocationId":"abc","status":"executed"}"""), out _));
        Assert.IsFalse(Decode<CommandProbeCatalog>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeCatalog, """{"revision":1,"commands":["ping","ping"]}"""), out _));
        Assert.IsFalse(Decode<CommandProbeCatalog>(CompanionProtocolTests.EnvelopeJson(3, CompanionMessageTypes.CommandProbeCatalog, """{"revision":0,"commands":[]}"""), out _));
    }

    [TestMethod]
    public void ServerIgnoresProbeInvocationsUnlessTheMarkerEnabledIt()
    {
        var withoutProbe = new Server(probe: null);
        withoutProbe.Negotiate(PeerA, NonceA);
        Assert.AreEqual(CompanionMessageOutcome.Ignored, withoutProbe.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "id1", "ping")).Outcome);

        var probe = new CommandProbeServer();
        var disabled = new Server(probe);
        disabled.Negotiate(PeerA, NonceA);
        Assert.IsFalse(probe.ApplyMarker(null));
        Assert.AreEqual(CompanionMessageOutcome.Ignored, disabled.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "id1", "ping")).Outcome);

        Assert.IsTrue(probe.ApplyMarker("catalog = ping"));
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, disabled.Handle(PeerA, PlayerA, Invoke(NonceA, 3, "id1", "ping")).Outcome);
    }

    [TestMethod]
    public void InvocationIsBoundToThePeerSessionAndSequence()
    {
        var probe = Enabled();
        var server = new Server(probe);
        server.Negotiate(PeerA, NonceA);
        server.Begin(PeerB, NonceB);

        var accepted = server.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "id1", "ping"));
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, accepted.Outcome);
        Assert.AreEqual(NonceA, accepted.SessionNonce);
        Assert.AreEqual("id1", accepted.CommandProbeInvocation!.InvocationId);

        // Wrong peer: peer B replays peer A's nonce; peer B before negotiating; an unknown peer.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerB, PlayerB, Invoke(NonceA, 9, "id2", "ping")).Outcome);
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerB, PlayerB, Invoke(NonceB, 2, "id3", "ping")).Outcome);
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(3003, PlayerB, Invoke(NonceA, 10, "id4", "ping")).Outcome);
        // Replayed or old sequence on the right session.
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "id5", "ping")).Outcome);

        // Reconnect: a new session for peer A; the old nonce is stale.
        server.Negotiate(PeerA, NonceA2);
        Assert.AreEqual(CompanionMessageOutcome.Ignored, server.Handle(PeerA, PlayerA, Invoke(NonceA, 20, "id6", "ping")).Outcome);
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, server.Handle(PeerA, PlayerA, Invoke(NonceA2, 2, "id6", "ping")).Outcome);
    }

    [TestMethod]
    public void RepeatedInvocationIdIsCountedAndNotSubmittedTwice()
    {
        var probe = Enabled();
        var server = new Server(probe);
        server.Negotiate(PeerA, NonceA);

        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, server.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "same", "ping")).Outcome);
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeDuplicate, server.Handle(PeerA, PlayerA, Invoke(NonceA, 3, "same", "ping")).Outcome);
        Assert.AreEqual(1, probe.DuplicateInvocations);

        // A new session starts a new id window.
        server.Negotiate(PeerA, NonceA2);
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, server.Handle(PeerA, PlayerA, Invoke(NonceA2, 2, "same", "ping")).Outcome);
    }

    [TestMethod]
    public void ServerRefusesCommandsOutsideItsCatalog()
    {
        var probe = Enabled();
        var server = new Server(probe);
        server.Negotiate(PeerA, NonceA);

        var result = server.Handle(PeerA, PlayerA, Invoke(NonceA, 2, "id1", "giveall"));
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeRejected, result.Outcome);
        Assert.AreEqual(CompanionMessageOutcome.CommandProbeSubmitted, server.Handle(PeerA, PlayerA, Invoke(NonceA, 3, "id2", "ping")).Outcome);
    }

    [TestMethod]
    public void MarkerCatalogGetsANewRevisionOnlyWhenItChanges()
    {
        var probe = new CommandProbeServer();
        Assert.IsTrue(probe.ApplyMarker("catalog = Ping, balance, bad name!, ping\nreceipts = on"));
        Assert.AreEqual(1, probe.Catalog!.Revision);
        CollectionAssert.AreEqual(new[] { "ping", "balance", "bad" }, probe.Catalog.Commands.ToArray());
        Assert.IsFalse(probe.ApplyMarker("catalog = Ping, balance, bad name!, ping\nreceipts = on"));

        Assert.IsTrue(probe.ApplyMarker("catalog = ping, balance, bad\nreceipts = off"));
        Assert.AreEqual(1, probe.Catalog.Revision);
        Assert.IsFalse(probe.ReceiptsEnabled);

        Assert.IsTrue(probe.ApplyMarker("catalog = ping, shop"));
        Assert.AreEqual(2, probe.Catalog.Revision);

        Assert.IsTrue(probe.ApplyMarker(null));
        Assert.IsFalse(probe.Enabled);
        Assert.AreEqual(3, probe.Catalog!.Revision, "disabling pushes a newer, empty catalog");
        Assert.AreEqual(0, probe.Catalog.Commands.Count);
        Assert.IsFalse(probe.ApplyMarker(null));
    }

    [TestMethod]
    public void ClientCatalogIsReplacedWholeAndOnlyForwardWithinASession()
    {
        var snapshot = new CommandProbeCatalogSnapshot();
        Assert.AreEqual(0, snapshot.CommandsFor(NonceA).Count);

        Assert.IsTrue(snapshot.TryReplace(NonceA, new CommandProbeCatalog(1, new[] { "ping" })));
        var before = snapshot.CommandsFor(NonceA);
        Assert.IsTrue(snapshot.TryReplace(NonceA, new CommandProbeCatalog(2, new[] { "ping", "balance" })));
        CollectionAssert.AreEqual(new[] { "ping" }, before.ToArray(), "a reader keeps the view it already took");
        CollectionAssert.AreEqual(new[] { "ping", "balance" }, snapshot.CommandsFor(NonceA).ToArray());

        Assert.IsFalse(snapshot.TryReplace(NonceA, new CommandProbeCatalog(2, new[] { "old" })));
        Assert.IsFalse(snapshot.TryReplace(NonceA, new CommandProbeCatalog(1, new[] { "older" })));
        Assert.AreEqual(0, snapshot.CommandsFor(NonceB).Count, "another session never sees this catalog");

        Assert.IsTrue(snapshot.TryReplace(NonceA2, new CommandProbeCatalog(1, new[] { "shop" })));
        CollectionAssert.AreEqual(new[] { "shop" }, snapshot.CommandsFor(NonceA2).ToArray());
        snapshot.Clear();
        Assert.IsNull(snapshot.Current);
    }

    [TestMethod]
    public void LineParserBoundsInput()
    {
        Assert.AreEqual(CommandProbeParseResult.Ok, CommandProbeLineParser.TryParse("TAKARO Ping  a b", out var command, out var arguments));
        Assert.AreEqual("ping", command);
        CollectionAssert.AreEqual(new[] { "a", "b" }, arguments.ToArray());
        Assert.AreEqual(CommandProbeParseResult.MissingCommand, CommandProbeLineParser.TryParse("takaro", out _, out _));
        Assert.AreEqual(CommandProbeParseResult.MissingCommand, CommandProbeLineParser.TryParse("takaro   ", out _, out _));
        Assert.AreEqual(CommandProbeParseResult.InvalidCommand, CommandProbeLineParser.TryParse("takaro p!ng", out _, out _));
        Assert.AreEqual(CommandProbeParseResult.TooManyArguments, CommandProbeLineParser.TryParse("takaro ping 1 2 3 4 5 6 7 8 9", out _, out _));
        Assert.AreEqual(CommandProbeParseResult.InvalidArgument, CommandProbeLineParser.TryParse("takaro ping " + new string('x', 65), out _, out _));
        Assert.AreEqual(CommandProbeParseResult.LineTooLong, CommandProbeLineParser.TryParse("takaro ping " + new string('x', 250), out _, out _));
    }

    [TestMethod]
    public void PendingInvocationsCompleteOnceExpireAndAreNeverReplayedAfterReset()
    {
        var pending = new CommandProbePendingInvocations();
        Assert.IsTrue(pending.TryAdd(NonceA, "id1", "ping", TimeSpan.Zero, context: null));
        Assert.IsFalse(pending.TryAdd(NonceA, "id1", "ping", TimeSpan.Zero, context: null));

        Assert.IsFalse(pending.TryComplete(NonceA2, "id1", out _), "a receipt from another session does not match");
        Assert.IsTrue(pending.TryComplete(NonceA, "id1", out var completed));
        Assert.AreEqual("ping", completed!.Command);
        Assert.IsFalse(pending.TryComplete(NonceA, "id1", out _), "a second receipt finds nothing");

        Assert.IsTrue(pending.TryAdd(NonceA, "id2", "ping", TimeSpan.Zero, context: null));
        Assert.AreEqual(0, pending.Expire(TimeSpan.FromSeconds(9)).Count);
        Assert.AreEqual(1, pending.Expire(CommandProbePendingInvocations.ReceiptTimeout).Count);
        Assert.AreEqual(0, pending.Count);

        Assert.IsTrue(pending.TryAdd(NonceA, "id3", "ping", TimeSpan.Zero, context: null));
        Assert.AreEqual(1, pending.Clear().Count);
        Assert.AreEqual(0, pending.Count);
        Assert.IsFalse(pending.TryComplete(NonceA, "id3", out _), "a late receipt after a reset is not applied");

        for (var i = 0; i < CommandProbePendingInvocations.Capacity; i++)
        {
            Assert.IsTrue(pending.TryAdd(NonceA, $"cap{i}", "ping", TimeSpan.Zero, context: null));
        }

        Assert.IsFalse(pending.TryAdd(NonceA, "overflow", "ping", TimeSpan.Zero, context: null));
    }

    [TestMethod]
    public void RegistrationNeverReplacesAndTeardownRemovesOnlyTheOwnedObject()
    {
        var native = new object();
        var modOwned = new object();
        var commands = new Dictionary<string, object> { ["say"] = native, ["ping"] = native, ["takaro"] = modOwned };

        Assert.IsFalse(CommandProbeRegistrationPolicy.CanRegister(commands, "takaro"), "mod-owned entry");
        Assert.IsFalse(CommandProbeRegistrationPolicy.CanRegister(commands, "say"), "native entry");
        Assert.IsFalse(CommandProbeRegistrationPolicy.CanRegister(commands, "Ping"), "mixed case maps to the lower-case key");
        Assert.IsFalse(CommandProbeRegistrationPolicy.TryRemoveOwned(commands, "takaro", new object()));
        Assert.AreSame(modOwned, commands["takaro"]);

        commands.Remove("takaro");
        Assert.IsTrue(CommandProbeRegistrationPolicy.CanRegister(commands, "takaro"));
        var ours = new object();
        commands["takaro"] = ours;
        Assert.IsTrue(CommandProbeRegistrationPolicy.StillOwned(commands, "TAKARO", ours));

        // Another mod overwrote ours later: teardown leaves theirs alone.
        var later = new object();
        commands["takaro"] = later;
        Assert.IsFalse(CommandProbeRegistrationPolicy.StillOwned(commands, "takaro", ours));
        Assert.IsFalse(CommandProbeRegistrationPolicy.TryRemoveOwned(commands, "takaro", ours));
        Assert.AreSame(later, commands["takaro"]);

        commands["takaro"] = ours;
        Assert.IsTrue(CommandProbeRegistrationPolicy.TryRemoveOwned(commands, "takaro", ours));
        Assert.IsFalse(commands.ContainsKey("takaro"));
        Assert.AreSame(native, commands["say"]);
        Assert.AreSame(native, commands["ping"]);
    }

    [TestMethod]
    public void ServerProbeEnvelopesMustCountUpWithinASession()
    {
        var sequence = new CommandProbeServerSequence();
        Assert.IsFalse(sequence.TryAccept(NonceA, 1), "1 is the hello");
        Assert.IsTrue(sequence.TryAccept(NonceA, 2));
        Assert.IsFalse(sequence.TryAccept(NonceA, 2));
        Assert.IsTrue(sequence.TryAccept(NonceA, 5));
        Assert.IsTrue(sequence.TryAccept(NonceA2, 2), "a new session starts over");
    }

    [TestMethod]
    public void ClientSettingsDefaultToTakaroAndRefuseInvalidRoots()
    {
        Assert.AreEqual(CommandProbeClientSettings.Default, CommandProbeClientSettings.Parse(""));
        Assert.AreEqual("say", CommandProbeClientSettings.Parse("root = Say").RootCommand);
        Assert.AreEqual("takaro", CommandProbeClientSettings.Parse("root = no spaces").RootCommand);
        Assert.IsTrue(CommandProbeClientSettings.Parse("console = on").EnableConsole);
    }

    [TestMethod]
    public void ClientSendsAnInvocationOnlyInsideANegotiatedSession()
    {
        var state = new CompanionClientState(CompanionProtocol.MinimumVersion, CompanionProtocol.CurrentVersion, CompanionCapability.Inventory);
        var invocation = new CommandProbeInvocation("id1", "ping", Array.Empty<string>());
        Assert.IsFalse(state.TryCreateReport(CompanionMessageTypes.CommandProbeInvoke, invocation, out _));

        var hello = new CompanionEnvelope(
            CompanionProtocol.NegotiationEnvelopeVersion,
            NonceA,
            1,
            "hello",
            CompanionMessageTypes.Hello,
            JsonSerializer.SerializeToElement(
                new CompanionHello(CompanionProtocol.MinimumVersion, CompanionProtocol.CurrentVersion, (int)CompanionCapability.Inventory),
                new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        Assert.IsTrue(state.TryPrepareHelloAck(hello, "1.0.0", out var prepared));
        Assert.IsTrue(state.ConfirmHelloAckSent(prepared!, TimeSpan.Zero));

        Assert.IsTrue(state.TryCreateReport(CompanionMessageTypes.CommandProbeInvoke, invocation, out var envelope));
        Assert.AreEqual(NonceA, envelope!.SessionNonce);
        StringAssert.DoesNotMatch(CompanionEnvelopeCodec.EncodeEnvelope(envelope), new System.Text.RegularExpressions.Regex("Steam_|player|role|permission"));
    }

    private static CommandProbeServer Enabled()
    {
        var probe = new CommandProbeServer();
        Assert.IsTrue(probe.ApplyMarker("catalog = ping"));
        return probe;
    }

    private static bool Decode<T>(string json, out T? payload)
        where T : class
    {
        payload = null;
        return CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _)
            && envelope is not null
            && CompanionEnvelopeCodec.TryDecodePayload(envelope, out payload, out _);
    }

    private static string Invoke(string nonce, long sequence, string id, string command, params string[] arguments) =>
        CompanionProtocolTests.EnvelopeJson(
            3,
            CompanionMessageTypes.CommandProbeInvoke,
            JsonSerializer.Serialize(new { invocationId = id, command, arguments }),
            nonce,
            sequence);

    private sealed class Server
    {
        public Server(CommandProbeServer? probe)
        {
            Sessions = new CompanionSessionRegistry(3, 3, CompanionCapability.Inventory, TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(30));
            Handler = new CompanionServerMessageHandler(
                Sessions,
                new CompanionRateLimiter(10, 5, TimeSpan.FromSeconds(1)),
                new CompanionRateLimiter(8, 4, TimeSpan.FromSeconds(1)),
                new CompanionInventoryCache(),
                new CompanionKillVerdictStore(),
                probe);
        }

        public CompanionSessionRegistry Sessions { get; }

        public CompanionServerMessageHandler Handler { get; }

        public void Begin(long peer, string nonce) => Sessions.Begin(peer, Now, nonce);

        public void Negotiate(long peer, string nonce)
        {
            Begin(peer, nonce);
            var ack = CompanionProtocolTests.EnvelopeJson(
                3,
                CompanionMessageTypes.HelloAck,
                """{"protocolVersion":3,"productVersion":"4.2.0","acceptedCapabilities":2}""",
                nonce,
                sequence: 1);
            Assert.AreEqual(CompanionMessageOutcome.Negotiated, Handle(peer, peer == PeerA ? PlayerA : PlayerB, ack).Outcome);
        }

        public CompanionMessageHandlingResult Handle(long peer, TakaroPlayer player, string json) =>
            Handler.Handle(peer, player, json, Now, Now);
    }
}
