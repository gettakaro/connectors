using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionProgressionTests
{
    private const long Peer = 1001;
    private const string Nonce = "nonce-a";
    private static readonly DateTimeOffset Now = DateTimeOffset.Parse("2026-11-05T10:00:00+00:00");
    private static readonly TakaroPlayer Player = new("Steam_76561190000000001", "Alice", "76561190000000001", "Steam_76561190000000001", null, null);
    private static readonly TakaroPlayer Listed = Player with { Character = new TakaroCharacter("7", "Alice") };
    private static readonly Regex TrackKey = new("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9-]*)*$");

    [TestMethod]
    public void AnUnknownPlayerReadsAsNullAndAConnectedPlayerWithoutTheModAsNoTracks()
    {
        var cache = new CompanionProgressionCache();
        Assert.IsNull(cache.Read(Player.GameId));

        cache.ObservePeer(Peer, Player);

        var tracks = cache.Read(Player.GameId);
        Assert.IsNotNull(tracks);
        Assert.AreEqual(0, tracks.Count);
    }

    [TestMethod]
    public void TheReadingDisappearsWithThePeer()
    {
        var cache = new CompanionProgressionCache();
        cache.Remember(Peer, Player, [new CompanionSkill("Jump", 3f, 0.5f, 0.2f)]);
        cache.ObservePlayerList([Listed]);
        Assert.AreEqual(1, cache.Read(Player.SteamId!)!.Count);

        cache.RemovePeer(Peer);

        Assert.IsNull(cache.Read(Player.GameId));
    }

    [TestMethod]
    public void ObservingAPeerAgainKeepsItsReading()
    {
        var cache = new CompanionProgressionCache();
        cache.Remember(Peer, Player, [new CompanionSkill("Jump", 3f, 0.5f, 0.2f)]);

        cache.ObservePeer(Peer, Player);

        cache.ObservePlayerList([Listed]);
        Assert.AreEqual(1, cache.Read(Player.GameId)!.Count);
    }

    [TestMethod]
    public void AnEndedSessionDropsTheReadingButKeepsThePlayerKnown()
    {
        var cache = new CompanionProgressionCache();
        cache.Remember(Peer, Player, [new CompanionSkill("Jump", 3f, 0.5f, 0.2f)]);

        cache.ForgetReading(Peer);

        cache.ObservePlayerList([Listed]);
        var tracks = cache.Read(Player.GameId);
        Assert.IsNotNull(tracks);
        Assert.AreEqual(0, tracks.Count);
    }

    [TestMethod]
    public void ForgettingAnUnknownPeerAddsNothing()
    {
        var cache = new CompanionProgressionCache();

        cache.ForgetReading(Peer);

        Assert.IsNull(cache.Read(Player.GameId));
    }

    [TestMethod]
    public void AFractionalLevelIsFlooredAndProgressIsTheBarNotTheFraction()
    {
        // A death left 23.7: the accumulator restarted, so nothing counts toward level 24 yet.
        var track = ValheimSkillTracks.ToTracks([new CompanionSkill("WoodCutting", 23.7f, 0f, 0f)]).Single();

        Assert.AreEqual("woodcutting", track.Key);
        Assert.AreEqual("Wood Cutting", track.Name);
        Assert.AreEqual(23, track.Level);
        Assert.AreEqual(0d, track.Progress);
        Assert.AreEqual(0d, track.Experience);
    }

    [TestMethod]
    public void ProgressAndExperienceComeFromTheGamesOwnCounters()
    {
        var track = ValheimSkillTracks.ToTracks([new CompanionSkill("Jump", 4f, 1.25f, 0.7f)]).Single();

        Assert.AreEqual(4, track.Level);
        Assert.AreEqual(1.25d, track.Experience);
        Assert.AreEqual(0.7d, track.Progress);
    }

    [TestMethod]
    public void ASkillOutsideTheTableIsLeftOutSoItCannotFailTheReading()
    {
        var tracks = ValheimSkillTracks.ToTracks(
        [
            new CompanionSkill("Swim", 1f, 0f, 0f),
            new CompanionSkill("not a valid key!", 99f, 0f, 0f),
            new CompanionSkill("None", 1f, 0f, 0f),
            new CompanionSkill("All", 1f, 0f, 0f)
        ]);

        CollectionAssert.AreEqual(new[] { "swim" }, tracks.Select(t => t.Key).ToArray());
    }

    [TestMethod]
    public void EverySkillMapsToAValidTrackAndNoneIsCalledCharacter()
    {
        string[] codes =
        [
            "Swords", "Knives", "Clubs", "Polearms", "Spears", "Blocking", "Axes", "Bows",
            "ElementalMagic", "BloodMagic", "Unarmed", "Pickaxes", "WoodCutting", "Crossbows",
            "Jump", "Sneak", "Run", "Swim", "Fishing", "Cooking", "Farming", "Crafting", "Dodge", "Ride"
        ];

        var tracks = ValheimSkillTracks.ToTracks(codes.Select(code => new CompanionSkill(code, 1f, 0f, 0f)).ToArray());

        Assert.AreEqual(codes.Length, tracks.Count);
        CollectionAssert.AllItemsAreUnique(tracks.Select(t => t.Key).ToArray());
        Assert.IsTrue(tracks.All(t => TrackKey.IsMatch(t.Key) && t.Key != "character"));
    }

    [TestMethod]
    public void TracksSerializeToTheProtocolFields()
    {
        var track = ValheimSkillTracks.ToTracks([new CompanionSkill("Ride", 2f, 0.5f, 0.25f)]).Single();

        var json = JsonSerializer.Serialize(track);

        Assert.AreEqual("""{"key":"ride","name":"Riding","level":2,"experience":0.5,"progress":0.25}""", json);
    }

    [TestMethod]
    public void AcceptedSkillsReachTheCacheBoundToThePeer()
    {
        var server = new Server();
        server.Negotiate(acceptedCapabilities: 6);

        var result = server.Handle(Skills(2, """{"code":"Jump","level":2.5,"accumulator":0.25,"progress":0.1}"""));

        Assert.AreEqual(CompanionMessageOutcome.SkillsAccepted, result.Outcome);
        server.Progression.ObservePlayerList([Listed]);
        Assert.AreEqual(2, server.Progression.Read(Player.GameId)!.Single().Level);
    }

    [TestMethod]
    public void ACompanionThatDidNotNegotiateSkillsIsIgnored()
    {
        var server = new Server();
        server.Negotiate(acceptedCapabilities: 2);

        var result = server.Handle(Skills(2, """{"code":"Jump","level":2,"accumulator":0,"progress":0}"""));

        Assert.AreEqual(CompanionMessageOutcome.Ignored, result.Outcome);
        Assert.IsNull(server.Progression.Read(Player.GameId));
    }

    [TestMethod]
    [DataRow("""{"code":"Jump","level":-1,"accumulator":0,"progress":0}""")]
    [DataRow("""{"code":"Jump","level":1,"accumulator":-1,"progress":0}""")]
    [DataRow("""{"code":"Jump","level":1,"accumulator":0,"progress":1.5}""")]
    [DataRow("""{"code":"Jump","level":2000000,"accumulator":0,"progress":0}""")]
    [DataRow("""{"code":"Jump","level":1,"accumulator":0,"progress":0,"extra":1}""")]
    [DataRow("""{"code":"Jump","level":1,"accumulator":0}""")]
    [DataRow("""{"code":"Jump","level":1,"accumulator":0,"progress":0},{"code":"Jump","level":2,"accumulator":0,"progress":0}""")]
    public void AMalformedReportIsDroppedAndTheLastGoodReadingStays(string skills)
    {
        var server = new Server();
        server.Negotiate(acceptedCapabilities: 6);
        server.Handle(Skills(2, """{"code":"Swim","level":3,"accumulator":0,"progress":0}"""));

        var result = server.Handle(Skills(3, skills));

        Assert.AreEqual(CompanionMessageOutcome.Ignored, result.Outcome);
        server.Progression.ObservePlayerList([Listed]);
        Assert.AreEqual("swim", server.Progression.Read(Player.GameId)!.Single().Key);
    }

    [TestMethod]
    public void ACompanionAcceptsOnlyTheCapabilitiesItSharesWithTheHello()
    {
        Assert.AreEqual(4, (int)CompanionCapability.Skills);
        var offered = CompanionCapability.Inventory & CompanionProtocol.KnownCapabilities;

        Assert.AreEqual(CompanionCapability.Inventory, offered, "An older server's hello must not gain a skills ack.");
    }

    [TestMethod]
    public void TheReaderSendsOnAChangeOrARefreshAndOtherwiseStaysQuiet()
    {
        var reader = new CompanionSkillsReader();
        var interval = TimeSpan.FromSeconds(20);
        CompanionSkill[] first = [new("Jump", 1f, 0.5f, 0.2f)];

        Assert.IsTrue(reader.TryGetChangedOrRefresh(first, TimeSpan.Zero, interval, out var report));
        reader.MarkSent(report!, TimeSpan.Zero);

        Assert.IsFalse(reader.TryGetChangedOrRefresh(first, TimeSpan.FromSeconds(2), interval, out _));
        Assert.IsTrue(reader.TryGetChangedOrRefresh([new("Jump", 1f, 0.75f, 0.3f)], TimeSpan.FromSeconds(4), interval, out _));
        Assert.IsTrue(reader.TryGetChangedOrRefresh(first, TimeSpan.FromSeconds(20), interval, out _));
    }

    [TestMethod]
    public void TheReaderSendsAnEmptyListOnceAndDropsUnusableEntries()
    {
        var reader = new CompanionSkillsReader();
        var interval = TimeSpan.FromSeconds(20);

        Assert.IsTrue(reader.TryGetChangedOrRefresh(
            [new("Jump", float.NaN, 0f, 0f), new("Run", float.PositiveInfinity, 0f, 0f), new("Sneak", 1f, float.PositiveInfinity, 0f), new("", 1f, 0f, 0f), new("Swim", 1f, 0f, 2f), new("Swim", 9f, 0f, 0f)],
            TimeSpan.Zero,
            interval,
            out var report));

        var skill = report!.Skills.Single();
        Assert.AreEqual("Swim", skill.Code);
        Assert.AreEqual(1f, skill.Level);
        Assert.AreEqual(1f, skill.Progress);

        var empty = new CompanionSkillsReader();
        Assert.IsTrue(empty.TryGetChangedOrRefresh(Array.Empty<CompanionSkill>(), TimeSpan.Zero, interval, out var emptyReport));
        Assert.AreEqual(0, emptyReport!.Skills.Count);
        Assert.IsFalse(empty.TryGetChangedOrRefresh(null, TimeSpan.Zero, interval, out _));
    }

    [TestMethod]
    public void TheManifestDeclaresProgressionAsClientReported()
    {
        var capability = ProtocolHandshake.BuildCapabilityManifest()
            .Single(c => c!["name"]!.GetValue<string>() == "players.progression")!;

        Assert.AreEqual("client-reported", capability["parameters"]!["source"]!.GetValue<string>());
        Assert.IsTrue(ProtocolHandshake.IsDeclared(TakaroActionNames.GetPlayerProgression));
    }

    [TestMethod]
    public void TheManifestDeclaresClientHeldCharactersThatDeathNeverEnds()
    {
        var capability = ProtocolHandshake.BuildCapabilityManifest()
            .Single(c => c!["name"]!.GetValue<string>() == "players.characters")!;

        Assert.AreEqual("client", capability["parameters"]!["held"]!.GetValue<string>());
        Assert.IsNull(capability["parameters"]!["ends"]);
    }

    [TestMethod]
    public void TheCharacterIsTheGamesIdAndNeverTheName()
    {
        var character = PlayerMapper.ToCharacter(-4242, "Bjorn", "Fallback")!;

        Assert.AreEqual("-4242", character.Id);
        Assert.AreEqual("Bjorn", character.Name);
        Assert.IsNull(PlayerMapper.ToCharacter(0, "Bjorn", "Fallback"));
        Assert.AreEqual("Fallback", PlayerMapper.ToCharacter(1, "", "Fallback")!.Name);
        Assert.AreEqual(255, PlayerMapper.ToCharacter(1, new string('x', 300), "Fallback")!.Name.Length);
    }

    [TestMethod]
    public void PlayerPayloadsAndEventsNameTheCharacterOnlyWhenKnown()
    {
        var withCharacter = Player with { Character = new TakaroCharacter("7", "Bjorn") };

        var known = JsonDocument.Parse(JsonSerializer.Serialize(EventFactory.PlayerConnected(withCharacter, Now))).RootElement;
        var unknown = JsonDocument.Parse(JsonSerializer.Serialize(EventFactory.PlayerConnected(Player, Now))).RootElement;

        Assert.AreEqual("7", known.GetProperty("player").GetProperty("character").GetProperty("id").GetString());
        Assert.IsFalse(unknown.GetProperty("player").TryGetProperty("character", out _));
    }

    [TestMethod]
    public void PlayerConnectedWaitsForTheCharacter()
    {
        var withCharacter = Player with { Character = new TakaroCharacter("7", "Bjorn") };

        Assert.IsFalse(PlayerLifecyclePresenceFilter.IsObserved(Player, hasPosition: true));
        Assert.IsFalse(PlayerLifecyclePresenceFilter.IsObserved(withCharacter, hasPosition: false));
        Assert.IsTrue(PlayerLifecyclePresenceFilter.IsObserved(withCharacter, hasPosition: true));
    }

    [TestMethod]
    public void ARelogOntoAnotherCharacterBetweenPollsStillReadsAsLeavingAndJoining()
    {
        var first = Player with { Character = new TakaroCharacter("7", "Bjorn") };
        var second = Player with { Character = new TakaroCharacter("8", "Astrid") };
        var tracker = new PlayerLifecycleEventTracker();
        tracker.Update([first], Now);

        var unreadable = tracker.Update([Player], Now);
        var switched = tracker.Update([second], Now);
        var settled = tracker.Update([second], Now);

        Assert.AreEqual(0, unreadable.Count);
        CollectionAssert.AreEqual(new[] { "player-disconnected", "player-connected" }, switched.Select(e => e.Type).ToArray());
        Assert.AreEqual("7", switched[0].Player.Character!.Id);
        Assert.AreEqual("8", switched[1].Player.Character!.Id);
        Assert.AreEqual(0, settled.Count);
    }

    [TestMethod]
    public void TracksStayBackUntilThePlayerListNamesTheCharacter()
    {
        var cache = new CompanionProgressionCache();
        cache.ObservePeer(Peer, Player);
        cache.Remember(Peer, Player, [new CompanionSkill("Jump", 3f, 0.5f, 0.2f)]);

        cache.ObservePlayerList([Player]);
        Assert.AreEqual(0, cache.Read(Player.GameId)!.Count);

        cache.ObservePlayerList([Player with { Character = new TakaroCharacter("7", "Bjorn") }]);
        Assert.AreEqual(1, cache.Read(Player.GameId)!.Count);

        cache.Remember(Peer, Player, [new CompanionSkill("Run", 3f, 0.5f, 0.2f)]);
        Assert.AreEqual("run", cache.Read(Player.GameId)!.Single().Key);
    }

    [TestMethod]
    public async Task TheActionNeverQueuesForTheGameLoop()
    {
        using var scheduler = new QueuedMainThreadActionScheduler(capacity: 2);
        var adapter = new ProgressionAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter, scheduler);

        var result = await dispatcher.DispatchAsync(new TakaroRequest(
            "1",
            TakaroActionNames.GetPlayerProgression,
            JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));

        Assert.IsTrue(result.Success);
        Assert.AreEqual("Steam_1", adapter.Identifier);
        Assert.AreEqual(0, scheduler.PendingCount);
    }

    private static string Skills(long sequence, string entries) =>
        CompanionProtocolTests.EnvelopeJson(
            3,
            CompanionMessageTypes.SkillsSnapshot,
            "{\"skills\":[" + entries + "]}",
            Nonce,
            sequence);

    private sealed class Server
    {
        private readonly CompanionSessionRegistry sessions = new(3, 3, CompanionProtocol.KnownCapabilities, TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(30));
        private readonly CompanionInventoryCache inventory = new();
        private readonly CompanionServerMessageHandler handler;

        public Server()
        {
            handler = new CompanionServerMessageHandler(
                sessions,
                new CompanionRateLimiter(10, 5, TimeSpan.FromSeconds(1)),
                new CompanionRateLimiter(8, 4, TimeSpan.FromSeconds(1)),
                inventory,
                new CompanionKillVerdictStore(),
                progression: Progression);
        }

        public CompanionProgressionCache Progression { get; } = new();

        public void Negotiate(int acceptedCapabilities)
        {
            sessions.Begin(Peer, Now, Nonce);
            inventory.BeginSession(Peer, Nonce);
            var ack = CompanionProtocolTests.EnvelopeJson(
                3,
                CompanionMessageTypes.HelloAck,
                $$"""{"protocolVersion":3,"productVersion":"4.6.0","acceptedCapabilities":{{acceptedCapabilities}}}""",
                Nonce,
                1);
            Assert.AreEqual(CompanionMessageOutcome.Negotiated, Handle(ack).Outcome);
        }

        public CompanionMessageHandlingResult Handle(string json) =>
            handler.Handle(Peer, Player, json, Now, Now);
    }

    private sealed class ProgressionAdapter : IValheimTakaroAdapter
    {
        public string? Identifier { get; private set; }

        public Task<TakaroActionResult> GetPlayerProgressionAsync(string identifier, CancellationToken cancellationToken = default)
        {
            Identifier = identifier;
            return Task.FromResult(TakaroActionResult.Ok());
        }

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, string? senderNameOverride, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> GetMapTileAsync(int x, int y, int z, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
    }
}
