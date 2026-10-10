using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro.Config;
using Takaro.Services;
using Takaro.WebSocket;

public static class ContractHarness
{
    private static int _assertions;

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length != 1)
                throw new ArgumentException("Expected the Generic Connector fixture path");

            JObject fixture = JObject.Parse(File.ReadAllText(args[0]));
            AssertBanExpiryConversion();
            AssertConsoleCommandOutcomeClassification();
            AssertResponseSerialization(fixture);
            AssertStableEventPayloads();
            AssertLegacyIdentityFieldsOnTheWire();
            AssertDisconnectedLocationReadWindow();
            AssertServerMessageEchoGuard();
            AssertWorldDtoSerialization(fixture);
            AssertPlayerProximateItemDelivery();
            AssertGiveItemProductionValidationAndCardinality();
            AssertProductionNotFoundReadSemantics();
            AssertNestedArgumentParsing();
            AssertRouterParsingAndCardinality(fixture);
            AssertControlFramesDoNotEnterRequestDispatch();
            AssertProtocolErrorsAreBoundedAndSafe();
            AssertIdentifyRejectionIsDetected();
            AssertMissingLocalisationIsNotShown();
            AssertOutboundLedgerReplaysUnconfirmedEvents();
            AssertCorrelatedMalformedRequestsTerminate();
            AssertRawRequestsAreNotLogged();
            AssertMapCatalog();
            AssertMapRouting();
            AssertConfigFiles();
            Console.WriteLine("Contract harness passed: " + _assertions + " assertions");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    // Takaro's PlayerService.resolveRef matches existing profiles on steamId,
    // epicOnlineServicesId, xboxLiveId and platformId, and never derives steamId from
    // platformId. Profiles from Takaro's built-in 7D2D integration have only the first
    // three, so every player object on the wire must carry them or Takaro duplicates the
    // player.
    private static void AssertLegacyIdentityFieldsOnTheWire()
    {
        ClientInfo steamClient = new ClientInfo
        {
            CrossplatformId = new PlatformUserIdentifierAbs
            {
                CombinedString = "EOS_000213691caa4c17ab8134415efd0889",
            },
            PlatformId = new PlatformUserIdentifierAbs
            {
                CombinedString = "Steam_76561198000000001",
            },
            playerName = "Steam Player",
            ip = "192.0.2.30",
            ping = 12,
        };
        foreach (
            KeyValuePair<string, Takaro.TakaroPlayer> shape in new Dictionary<
                string,
                Takaro.TakaroPlayer
            >
            {
                {
                    "connected player",
                    Takaro.Shared.TransformClientInfoToTakaroPlayer(steamClient)
                },
                {
                    "event identity",
                    Takaro.Shared.TransformClientInfoToTakaroPlayerIdentity(steamClient)
                },
                {
                    "mirror record",
                    Takaro.Shared.TransformPlayerRecordToTakaroPlayer(
                        new Takaro.Persistence.PlayerRecord
                        {
                            GameId = "000213691caa4c17ab8134415efd0889",
                            Name = "Steam Player",
                            SteamId = "76561198000000001",
                            EpicOnlineServicesId = "000213691caa4c17ab8134415efd0889",
                        }
                    )
                },
                {
                    "ban player",
                    Takaro
                        .Shared.TransformBanRecordToTakaroBan(
                            new Takaro.Persistence.BanRecord
                            {
                                GameId = "000213691caa4c17ab8134415efd0889",
                                Name = "Steam Player",
                                SteamId = "76561198000000001",
                                EpicOnlineServicesId = "000213691caa4c17ab8134415efd0889",
                                Reason = "fixture",
                            }
                        )
                        .Player
                },
            }
        )
        {
            JObject json = JObject.Parse(JsonConvert.SerializeObject(shape.Value));
            Equal(
                "000213691caa4c17ab8134415efd0889",
                (string)json["gameId"],
                shape.Key + " gameId stays the EOS id"
            );
            Equal("76561198000000001", (string)json["steamId"], shape.Key + " carries steamId");
            Equal(
                "000213691caa4c17ab8134415efd0889",
                (string)json["epicOnlineServicesId"],
                shape.Key + " carries epicOnlineServicesId"
            );
            Equal(
                "steam:76561198000000001",
                (string)json["platformId"],
                shape.Key + " keeps platformId"
            );
            True(json["xboxLiveId"] == null, shape.Key + " omits an unknown xboxLiveId");
        }

        JObject xbox = JObject.Parse(
            JsonConvert.SerializeObject(
                Takaro.Shared.TransformClientInfoToTakaroPlayer(
                    new ClientInfo
                    {
                        CrossplatformId = new PlatformUserIdentifierAbs
                        {
                            CombinedString = "EOS_fixture-xbox-eos",
                        },
                        PlatformId = new PlatformUserIdentifierAbs
                        {
                            CombinedString = "XBL_2535400000000001",
                        },
                        playerName = "Xbox Player",
                    }
                )
            )
        );
        Equal("2535400000000001", (string)xbox["xboxLiveId"], "Xbox player carries xboxLiveId");
        Equal(
            "fixture-xbox-eos",
            (string)xbox["epicOnlineServicesId"],
            "Xbox player carries epicOnlineServicesId"
        );
        Equal("xbox:2535400000000001", (string)xbox["platformId"], "Xbox player platformId");
        True(xbox["steamId"] == null, "Xbox player omits steamId");

        JObject eosOnly = JObject.Parse(
            JsonConvert.SerializeObject(
                Takaro.Shared.TransformClientInfoToTakaroPlayerIdentity(
                    new ClientInfo
                    {
                        CrossplatformId = new PlatformUserIdentifierAbs
                        {
                            CombinedString = "EOS_fixture-eos-only",
                        },
                        PlatformId = new PlatformUserIdentifierAbs { CombinedString = "" },
                        playerName = "EOS Player",
                    }
                )
            )
        );
        Equal(
            "fixture-eos-only",
            (string)eosOnly["epicOnlineServicesId"],
            "EOS-only player carries epicOnlineServicesId"
        );
        Equal("eos:fixture-eos-only", (string)eosOnly["platformId"], "EOS-only platformId");
        True(
            eosOnly["steamId"] == null && eosOnly["xboxLiveId"] == null,
            "EOS-only player sends no empty steamId or xboxLiveId"
        );

        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendPlayerDisconnected(
            Takaro.Shared.TransformClientInfoToTakaroPlayerIdentity(steamClient)
        );
        AssertPublishedEvent("player-disconnected", out JObject disconnectedData);
        Equal(
            "76561198000000001",
            (string)disconnectedData["player"]["steamId"],
            "player-disconnected frame carries steamId"
        );
        Equal(
            "000213691caa4c17ab8134415efd0889",
            (string)disconnectedData["player"]["epicOnlineServicesId"],
            "player-disconnected frame carries epicOnlineServicesId"
        );
    }

    private static void AssertDisconnectedLocationReadWindow()
    {
        DateTime now = DateTime.Parse("2026-07-24T10:24:12Z").ToUniversalTime();

        True(
            PlayerLocationReadWindow.IsReadable(true, DateTime.MinValue, now),
            "online player location is readable"
        );
        True(
            PlayerLocationReadWindow.IsReadable(false, now.AddSeconds(-1), now),
            "recent disconnect location remains readable for Takaro enrichment"
        );
        True(
            PlayerLocationReadWindow.IsReadable(false, now.AddMinutes(-10), now),
            "disconnect location remains readable at the grace boundary"
        );
        True(
            !PlayerLocationReadWindow.IsReadable(false, now.AddMinutes(-10).AddSeconds(-1), now),
            "stale offline player location is not exposed"
        );

        TimeZoneInfo utcPlusTwo = TimeZoneInfo.CreateCustomTimeZone(
            "fixture-utc-plus-two",
            TimeSpan.FromHours(2),
            "Fixture UTC+2",
            "Fixture UTC+2"
        );
        DateTime localWallTime = DateTime.SpecifyKind(
            now.AddHours(2).AddSeconds(-1),
            DateTimeKind.Unspecified
        );
        True(
            PlayerLocationReadWindow.IsReadable(false, localWallTime, now, utcPlusTwo),
            "LiteDB local wall time is normalized before disconnect age comparison"
        );
    }

    private static void AssertServerMessageEchoGuard()
    {
        var guard = new ServerMessageEchoGuard();
        DateTimeOffset now = DateTimeOffset.Parse("2026-07-24T09:20:25Z");
        const string outbound = "{\"msg\":\"TESTING native log\",\"timestamp\":\"fixture\"}";
        string nativeEcho = "Chat (from '-non-player-', entity id '-1', to 'Global'): " + outbound;

        guard.Record(outbound, now);
        True(
            guard.ShouldSuppress(nativeEcho, now.AddSeconds(1)),
            "recent Takaro server-message echo is suppressed"
        );
        True(
            !guard.ShouldSuppress(nativeEcho, now.AddSeconds(1)),
            "server-message echo suppression is consumed exactly once"
        );

        guard.Record("ordinary announcement", now);
        True(
            !guard.ShouldSuppress(
                "Chat (from 'Fixture Player', entity id '7', to 'Global'): ordinary announcement",
                now.AddSeconds(1)
            ),
            "player chat with matching text is not suppressed"
        );

        guard.Record("ordinary announcement", now);
        True(
            !guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): prefix: ordinary announcement",
                now.AddSeconds(1)
            ),
            "non-player suffix collision is not suppressed"
        );
        True(
            guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): ordinary announcement",
                now.AddSeconds(1)
            ),
            "suffix collision does not consume the exact global echo"
        );

        guard.Record("private announcement", now);
        True(
            !guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Whisper'): private announcement",
                now.AddSeconds(1)
            ),
            "non-global server chat is not suppressed"
        );
        True(
            !guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): ordinary announcement",
                now.AddSeconds(31)
            ),
            "expired server-message echoes are not suppressed"
        );

        for (int index = 0; index < ServerMessageEchoGuard.MaxEntries + 1; index++)
            guard.Record("message-" + index, now);
        True(
            !guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): message-0",
                now.AddSeconds(1)
            ),
            "server-message echo guard stays bounded"
        );
        True(
            guard.ShouldSuppress(
                "Chat (from '-non-player-', entity id '-1', to 'Global'): message-128",
                now.AddSeconds(1)
            ),
            "server-message echo guard retains the newest bounded entry"
        );
    }

    private static void AssertStableEventPayloads()
    {
        var client = new ClientInfo
        {
            CrossplatformId = new PlatformUserIdentifierAbs
            {
                CombinedString = "EOS_fixture-event-player",
            },
            PlatformId = new PlatformUserIdentifierAbs
            {
                CombinedString = "Steam_fixture-event-platform",
            },
            playerName = "Fixture Event Player",
            ip = "192.0.2.25",
            ping = 73,
        };
        Takaro.TakaroPlayer identity = Takaro.Shared.TransformClientInfoToTakaroPlayerIdentity(
            client
        );
        JObject identityJson = JObject.Parse(JsonConvert.SerializeObject(identity));
        Equal("fixture-event-player", (string)identityJson["gameId"], "identity gameId");
        Equal("Fixture Event Player", (string)identityJson["name"], "identity name");
        Equal(
            "steam:fixture-event-platform",
            (string)identityJson["platformId"],
            "identity platformId"
        );
        True(identityJson["ip"] == null, "identity snapshot omits teardown IP");
        True(identityJson["ping"] == null, "identity snapshot omits teardown ping");

        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendPlayerDisconnected(identity);
        AssertPublishedEvent("player-disconnected", out JObject disconnectedData);
        TokenEqual(identityJson, disconnectedData["player"], "disconnect uses stable identity");

        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendPlayerDeath(
            identity,
            null,
            new UnityEngine.Vector3(10.5f, 20.25f, 30.75f)
        );
        AssertPublishedEvent("player-death", out JObject deathData);
        TokenEqual(identityJson, deathData["player"], "death uses stable identity");
        True(deathData["attacker"] == null, "death omits missing attacker");
        Equal(10.5f, (float)deathData["position"]["x"], "death position x");
        Equal(20.25f, (float)deathData["position"]["y"], "death position y");
        Equal(30.75f, (float)deathData["position"]["z"], "death position z");

        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendEntityKilled(identity, "Rabbit", null);
        AssertPublishedEvent("entity-killed", out JObject killedData);
        TokenEqual(identityJson, killedData["player"], "entity kill uses stable identity");
        Equal("Rabbit", (string)killedData["entity"], "entity kill uses the display name");
        Equal("unknown", (string)killedData["weapon"], "entity kill weapon fallback");

        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendEntityKilled(identity, null, "Steel Club");
        AssertPublishedEvent("entity-killed", out JObject unnamedKill);
        Equal("unknown", (string)unnamedKill["entity"], "entity kill name fallback");
        Equal("Steel Club", (string)unnamedKill["weapon"], "entity kill weapon name");
    }

    private static void AssertIdentifyRejectionIsDetected()
    {
        // Verbatim shape of Takaro's reply to a stale registration token.
        string rejected =
            "{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"BadRequestError\","
            + "\"message\":\"Invalid registrationToken provided\",\"http\":400}},\"requestId\":\"r1\"}";
        True(
            ProtocolDiagnostics.TryGetIdentifyRejection(rejected, out string reason),
            "identify error is a rejection"
        );
        Equal("Invalid registrationToken provided", reason, "identify rejection reason");

        string accepted =
            "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-1\"},\"requestId\":\"r2\"}";
        True(
            !ProtocolDiagnostics.TryGetIdentifyRejection(accepted, out _),
            "identify with gameServerId is accepted"
        );
        True(
            !ProtocolDiagnostics.TryGetIdentifyRejection(
                "{\"type\":\"identifyResponse\",\"payload\":{\"error\":null}}",
                out _
            ),
            "null error is not a rejection"
        );
        True(
            !ProtocolDiagnostics.TryGetIdentifyRejection(
                "{\"type\":\"pong\",\"payload\":{\"error\":{\"message\":\"x\"}}}",
                out _
            ),
            "only identifyResponse can reject identify"
        );
        True(
            !ProtocolDiagnostics.TryGetIdentifyRejection("not json", out _),
            "malformed frame is not a rejection"
        );
        True(
            ProtocolDiagnostics.TryGetIdentifyRejection(
                "{\"type\":\"identifyResponse\",\"payload\":{\"error\":\"plain text reason\"}}",
                out string plain
            )
                && plain == "plain text reason",
            "string error is a rejection with its text"
        );
    }

    private static void AssertMissingLocalisationIsNotShown()
    {
        Equal<string>(
            null,
            Takaro.Shared.LocalizedOrNull("driftwoodDesc", "driftwoodDesc"),
            "untranslated key is hidden"
        );
        Equal<string>(
            null,
            Takaro.Shared.LocalizedOrNull("woodMaster", ""),
            "empty localisation is hidden"
        );
        Equal(
            "Steel Club",
            Takaro.Shared.LocalizedOrNull("meleeWpnClubT3SteelClub", "Steel Club"),
            "real name is kept"
        );
    }

    private static void AssertOutboundLedgerReplaysUnconfirmedEvents()
    {
        var ledger = new OutboundLedger(5, 4);
        ledger.RequeueInFlight(1);

        ledger.Enqueue("e1", true, false);
        ledger.Enqueue("ping1", false, true);
        ledger.Enqueue("e2", true, false);
        ledger.Enqueue("resp", false, false);
        for (int i = 0; i < 4; i++)
            ledger.MarkHeadWritten();
        Equal(2, ledger.InFlightCount, "written events stay in flight, responses and pings do not");

        Equal(1, ledger.AcknowledgePong(), "pong confirms only events written before its ping");
        Equal(1, ledger.InFlightCount, "event written after the ping is still unconfirmed");
        Equal(0, ledger.AcknowledgePong(), "a pong without an outstanding ping confirms nothing");

        // The socket dies: the unconfirmed event goes back in front of newer traffic.
        ledger.Enqueue("e3", true, false);
        Equal(1, ledger.RequeueInFlight(2), "unconfirmed event is requeued");
        Equal(2L, ledger.Generation, "requeue starts a new generation");
        Equal("e2", ledger.PeekHead().Json, "requeued event keeps its place before newer events");
        ledger.MarkHeadWritten();
        Equal("e3", ledger.PeekHead().Json, "newer event follows the requeued one");
        ledger.MarkHeadWritten();
        Equal(0, ledger.AcknowledgePong(), "pings from the dead socket do not confirm anything");
        Equal(2, ledger.InFlightCount, "events on the new socket wait for a new pong");

        var acked = new OutboundLedger(10, 10);
        acked.RequeueInFlight(1);
        True(!acked.NeedsAckPing, "nothing written needs no ack ping");
        acked.Enqueue("a", true, false);
        acked.MarkHeadWritten();
        True(acked.NeedsAckPing, "a written event needs an ack ping");
        acked.Enqueue("p", false, true);
        acked.MarkHeadWritten();
        True(!acked.NeedsAckPing, "a ping after the event covers it");
        Equal(1, acked.AcknowledgePong(), "its pong confirms the event");
        acked.Enqueue("r", false, false);
        acked.MarkHeadWritten();
        True(!acked.NeedsAckPing, "responses never need an ack ping");

        var capped = new OutboundLedger(3, 2);
        for (int i = 0; i < 4; i++)
            capped.Enqueue("x" + i, true, false);
        Equal(3, capped.PendingCount, "backlog is capped");
        Equal("x1", capped.PeekHead().Json, "oldest frame is dropped first");
        for (int i = 0; i < 3; i++)
            capped.MarkHeadWritten();
        Equal(2, capped.InFlightCount, "in-flight list is capped");
        Equal(2, capped.RequeueInFlight(1), "capped in-flight events are requeued");
        Equal("x2", capped.PeekHead().Json, "requeue keeps the newest unconfirmed events in order");
    }

    private static void AssertPublishedEvent(string expectedType, out JObject eventData)
    {
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            expectedType + " publishes exactly one frame"
        );
        JObject frame = JObject.Parse(
            JsonConvert.SerializeObject(WebSocketTransport.Instance.TerminalMessages[0])
        );
        Equal("gameEvent", (string)frame["type"], expectedType + " frame type");
        Equal(expectedType, (string)frame["payload"]["type"], expectedType + " event type");
        eventData = (JObject)frame["payload"]["data"];
        True(eventData != null, expectedType + " has event data");
    }

    private static void AssertBanExpiryConversion()
    {
        TimeZoneInfo utcPlusTwo = TimeZoneInfo.CreateCustomTimeZone(
            "Fixture UTC+02",
            TimeSpan.FromHours(2),
            "Fixture UTC+02",
            "Fixture UTC+02"
        );
        DateTimeOffset utcNow = DateTimeOffset.Parse("2026-07-23T20:00:00Z");

        True(
            BanExpiry.TryCreateGameDeadline(
                "2026-07-23T20:15:00Z",
                utcNow,
                utcPlusTwo,
                out DateTime gameDeadline,
                out string error
            ),
            "future Takaro UTC expiry is accepted"
        );
        Equal(string.Empty, error, "accepted expiry has no error");
        Equal(
            new DateTime(2026, 7, 23, 22, 15, 0, DateTimeKind.Unspecified),
            gameDeadline,
            "UTC expiry becomes the game-local wall clock"
        );
        Equal(
            "2026-07-23T20:15:00.0000000Z",
            BanExpiry.ToTakaroUtc(gameDeadline, utcPlusTwo),
            "game-local deadline round-trips to canonical Takaro UTC"
        );

        True(
            !BanExpiry.TryCreateGameDeadline(
                "not-a-timestamp",
                utcNow,
                utcPlusTwo,
                out DateTime invalidDeadline,
                out string invalidError
            ),
            "invalid Takaro expiry is rejected"
        );
        True(!string.IsNullOrEmpty(invalidError), "invalid expiry returns an error");

        True(
            !BanExpiry.TryCreateGameDeadline(
                "2026-07-23T20:00:00Z",
                utcNow,
                utcPlusTwo,
                out DateTime currentDeadline,
                out string currentError
            ),
            "current Takaro expiry is rejected"
        );
        True(!string.IsNullOrEmpty(currentError), "current expiry returns an error");

        True(
            !BanExpiry.TryCreateGameDeadline(
                "2026-07-23T19:59:59Z",
                utcNow,
                utcPlusTwo,
                out DateTime pastDeadline,
                out string pastError
            ),
            "past Takaro expiry is rejected"
        );
        True(!string.IsNullOrEmpty(pastError), "past expiry returns an error");
    }

    private static void AssertConsoleCommandOutcomeClassification()
    {
        ConsoleCommandOutcome valid = ConsoleCommandOutcome.FromRawResult(
            "Game version: V3.0.1\nDay 4, 12:00"
        );
        True(valid.Success, "ordinary multiline console output succeeds");
        Equal(
            "Game version: V3.0.1\nDay 4, 12:00",
            valid.RawResult,
            "successful console output remains byte-for-byte unchanged"
        );
        True(valid.ErrorMessage == null, "successful console output omits error message");

        foreach (
            string rawResult in new[]
            {
                "*** ERROR: Unknown command",
                "Command preface\r\n*** ERROR: Native rejection\r\nCommand suffix",
                "Wrong number of arguments, expected 2, found 0.",
                "Invalid value for single argument variant: \"not-a-time\"",
            }
        )
        {
            ConsoleCommandOutcome rejected = ConsoleCommandOutcome.FromRawResult(rawResult);
            True(!rejected.Success, "native failure line rejects console command");
            Equal(rawResult, rejected.RawResult, "rejected console output remains unchanged");
            True(
                rawResult.Contains(rejected.ErrorMessage),
                "rejected console output exposes the first native failure line"
            );
        }

        foreach (
            string rawResult in new[]
            {
                " *** ERROR: indented text is not a native error line",
                "prefix *** ERROR: embedded text is not a native error line",
                "*** Error: matching is ordinal and case-sensitive",
            }
        )
        {
            True(
                ConsoleCommandOutcome.FromRawResult(rawResult).Success,
                "near-match console output remains successful"
            );
        }

        ConsoleCommandOutcome bounded = ConsoleCommandOutcome.FromRawResult(
            "*** ERROR: " + new string('x', 2048)
        );
        True(
            bounded.ErrorMessage.Length <= ConsoleCommandOutcome.MaxErrorMessageLength,
            "native error message is bounded"
        );
    }

    private static void AssertPlayerProximateItemDelivery()
    {
        GameManager.Instance.ResetItemDrops();
        var itemValue = new ItemValue(42, true) { Quality = 3 };
        var player = new EntityPlayer(73, new UnityEngine.Vector3(10.5f, 20.25f, 30.75f));
        var client = new ClientInfo { entityId = 73 };

        True(
            PlayerProximateItemDelivery.Deliver(itemValue, 7, player, client),
            "delivery to a connected player goes into the inventory"
        );

        World world = GameManager.Instance.World;
        Equal(0, GameManager.Instance.ItemDrops.Count, "inventory delivery leaves no ground drop");
        Equal(1, world.Spawned.Count, "delivery spawns exactly one item entity");
        EntityItem spawned = world.Spawned[0];
        EntityCreationData data = spawned.CreationData;
        Equal(42, data.itemStack.itemValue.type, "delivery preserves item type");
        Equal((ushort)3, data.itemStack.itemValue.Quality, "delivery preserves item quality");
        Equal(7, data.itemStack.count, "delivery creates one stack with the requested amount");
        Equal(73, data.belongsPlayerId, "the item entity belongs to the receiving player");
        Equal(10.5f, data.pos.x, "item entity spawns at the player x");
        Equal(30.75f, data.pos.z, "item entity spawns at the player z");
        Equal(1, client.SentPackages.Count, "exactly one package goes to the receiving client");
        var collect = client.SentPackages[0] as NetPackageEntityCollect;
        True(collect != null, "the package is an entity collect");
        Equal(spawned.entityId, collect.EntityId, "collect names the spawned item entity");
        Equal(73, collect.PlayerId, "collect is for the receiving player");
        Equal(1, world.Removed.Count, "the item entity is removed after the collect");
        Equal(spawned.entityId, world.Removed[0], "the removed entity is the spawned one");

        GameManager.Instance.ResetItemDrops();
        True(
            !PlayerProximateItemDelivery.Deliver(itemValue, 2, player, null),
            "without a client the delivery falls back"
        );
        Equal(1, GameManager.Instance.ItemDrops.Count, "fallback drops one stack at the player");
        Equal(2, GameManager.Instance.ItemDrops[0].Stack.count, "fallback keeps the amount");
        Equal(0, world.Spawned.Count, "fallback spawns no collect entity");
    }

    private static void AssertGiveItemProductionValidationAndCardinality()
    {
        GameManager.Instance.ResetGiveItemFixture();

        AssertGiveItemTerminal(
            GiveItemArgs("missing-player", "resourceWood", 2, "1"),
            WebSocketMessage.MessageTypes.Error,
            "invalid player"
        );
        AssertGiveItemTerminal(
            GiveItemArgs("fixture-player", "missing-item", 2, "1"),
            WebSocketMessage.MessageTypes.Error,
            "invalid item"
        );
        AssertGiveItemTerminal(
            GiveItemArgs("fixture-player", "resourceWood", 0, "1"),
            WebSocketMessage.MessageTypes.Error,
            "invalid amount"
        );
        AssertGiveItemTerminal(
            GiveItemArgs("fixture-player", "resourceWood", 2, "999"),
            WebSocketMessage.MessageTypes.Error,
            "invalid quality"
        );
        AssertGiveItemTerminal(
            GiveItemArgs("fixture-player", "resourceWood", 2, "1"),
            WebSocketMessage.MessageTypes.Response,
            "valid giveItem"
        );
        Equal(0, GameManager.Instance.ItemDrops.Count, "valid giveItem leaves no ground drop");
        Equal(
            1,
            GameManager.Instance.World.Spawned.Count,
            "valid giveItem spawns one collect entity"
        );
        Equal(
            2,
            GameManager.Instance.World.Spawned[0].CreationData.itemStack.count,
            "valid giveItem preserves amount"
        );
        Equal(
            1,
            ConnectionManager.Instance.Clients.FixtureClient.SentPackages.Count,
            "valid giveItem tells the receiving client to collect it"
        );
    }

    private static TakaroGiveItemArgs GiveItemArgs(
        string gameId,
        string item,
        int amount,
        string quality
    )
    {
        return new TakaroGiveItemArgs
        {
            Player = new TakaroPlayerReference { GameId = gameId },
            Item = item,
            Amount = amount,
            Quality = quality,
        };
    }

    private static void AssertGiveItemTerminal(
        TakaroGiveItemArgs args,
        string expectedType,
        string description
    )
    {
        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameManager.Instance.ResetItemDrops();
        string requestId = "give-item-" + description.Replace(" ", "-");

        GiveItemHandler.Handle(requestId, args).GetAwaiter().GetResult();

        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            description + " has exactly one terminal response"
        );
        WebSocketMessage terminal = WebSocketTransport.Instance.TerminalMessages[0];
        Equal(expectedType, terminal.Type, description + " terminal type");
        Equal(requestId, terminal.RequestId, description + " preserves requestId");
        if (expectedType == WebSocketMessage.MessageTypes.Response)
        {
            JObject serialized = JObject.Parse(JsonConvert.SerializeObject(terminal));
            True(serialized["payload"].Type == JTokenType.Null, description + " payload is null");
        }
    }

    private static void AssertResponseSerialization(JObject fixture)
    {
        JObject nullResponse = JObject.Parse(
            JsonConvert.SerializeObject(WebSocketMessage.CreateResponse("fixture-request", null))
        );
        Equal("response", (string)nullResponse["type"], "response type");
        Equal("fixture-request", (string)nullResponse["requestId"], "response requestId");
        True(nullResponse["payload"].Type == JTokenType.Null, "null payload stays JSON null");

        Takaro.TakaroPlayer player = Takaro.Shared.TransformPlayerRecordToTakaroPlayer(
            new Takaro.Persistence.PlayerRecord
            {
                GameId = "fixture-player",
                Name = "Fixture Player",
                Ping = 42,
                SteamId = "fixture-player",
                EpicOnlineServicesId = "fixture-player",
            }
        );
        JToken playerResponse = JToken.Parse(
            JsonConvert.SerializeObject(WebSocketMessage.CreateResponse("fixture-request", player))
        );
        JToken expectedPlayerResponse = Function(fixture, "getPlayer")["response"];
        TokenEqual(expectedPlayerResponse, playerResponse, "complete getPlayer response fixture");

        Takaro.TakaroPlayer connectedPlayer = Takaro.Shared.TransformClientInfoToTakaroPlayer(
            new ClientInfo
            {
                CrossplatformId = new PlatformUserIdentifierAbs
                {
                    CombinedString = "EOS_fixture-player",
                },
                PlatformId = new PlatformUserIdentifierAbs
                {
                    CombinedString = "Steam_fixture-player",
                },
                playerName = "Fixture Player",
                ping = 42,
            }
        );
        JToken connectedPlayerResponse = JToken.Parse(
            JsonConvert.SerializeObject(
                WebSocketMessage.CreateResponse("fixture-request", connectedPlayer)
            )
        );
        TokenEqual(
            expectedPlayerResponse,
            connectedPlayerResponse,
            "complete connected-player response fixture"
        );

        Takaro.TakaroBan ban = Takaro.Shared.TransformBanRecordToTakaroBan(
            new Takaro.Persistence.BanRecord
            {
                GameId = "fixture-player",
                Name = "Fixture Player",
                SteamId = "fixture-player",
                EpicOnlineServicesId = "fixture-player",
                Reason = "qualification fixture",
                ExpiresAt = "2030-01-01T00:00:00Z",
            }
        );
        JToken banResponse = JToken.Parse(
            JsonConvert.SerializeObject(
                WebSocketMessage.CreateResponse("fixture-request", new[] { ban })
            )
        );
        JToken expectedBanResponse = Function(fixture, "listBans")["response"];
        TokenEqual(expectedBanResponse, banResponse, "complete nested ban response fixture");

        Equal(
            "steam:fixture-player",
            Takaro.Shared.PlatformIdFromIdentifiers("fixture-player", null, "fixture-eos"),
            "Steam native identifier has priority"
        );
        Equal(
            "xbox:fixture-player",
            Takaro.Shared.PlatformIdFromIdentifiers(null, "fixture-player", "fixture-eos"),
            "Xbox native identifier is normalized"
        );
        Equal(
            "eos:fixture-eos",
            Takaro.Shared.PlatformIdFromIdentifiers(null, null, "fixture-eos"),
            "EOS cross-platform identifier is the fallback"
        );
        Equal(
            "steam:fixture-player",
            Takaro
                .Shared.TransformPlayerRecordToTakaroPlayer(
                    new Takaro.Persistence.PlayerRecord
                    {
                        GameId = "fixture-player",
                        Name = "Fixture Player",
                        SteamId = "fixture-player",
                        EpicOnlineServicesId = "fixture-eos",
                    }
                )
                .PlatformId,
            "PlayerRecord native identifier maps to platformId"
        );
        Equal(
            "steam:fixture-player",
            Takaro
                .Shared.TransformClientInfoToTakaroPlayer(
                    new ClientInfo
                    {
                        CrossplatformId = new PlatformUserIdentifierAbs
                        {
                            CombinedString = "EOS_fixture-eos",
                        },
                        PlatformId = new PlatformUserIdentifierAbs
                        {
                            CombinedString = "Steam_fixture-player",
                        },
                        playerName = "Fixture Player",
                    }
                )
                .PlatformId,
            "ClientInfo native identifier maps to platformId"
        );
    }

    private static void AssertNestedArgumentParsing()
    {
        const string giveJson =
            "{\"player\":{\"gameId\":\"fixture-player\"},\"item\":\"resourceWood\",\"amount\":2,\"quality\":\"1\"}";
        TakaroGiveItemArgs give = WebSocketArgs<TakaroGiveItemArgs>.Parse(giveJson);
        Equal("fixture-player", give.Player.GameId, "giveItem nested player JSON string");
        Equal("resourceWood", give.Item, "giveItem item JSON string");
        Equal(2, give.Amount, "giveItem amount JSON string");

        JObject messageObject = JObject.Parse(
            "{\"message\":\"fixture\",\"opts\":{\"recipient\":{\"gameId\":\"fixture-player\"},\"senderNameOverride\":\"Bot\"}}"
        );
        TakaroSendMessageArgs message = WebSocketArgs<TakaroSendMessageArgs>.Parse(messageObject);
        Equal(
            "fixture-player",
            message.Opts.Recipient.GameId,
            "sendMessage nested recipient JObject"
        );
        Equal("Bot", message.Opts.SenderNameOverride, "sendMessage sender override JObject");

        var directUnban = new Dictionary<string, object> { { "gameId", "fixture-player" } };
        TakaroUnbanPlayerArgs unban = WebSocketArgs<TakaroUnbanPlayerArgs>.Parse(directUnban);
        Equal("fixture-player", unban.GameId, "unban direct gameId dictionary");
    }

    private static void AssertProductionNotFoundReadSemantics()
    {
        AssertSingleTerminalError(
            () => ReadHandlers.GetPlayer("fixture-missing-player", "missing-player"),
            "missing getPlayer"
        );
        AssertSingleTerminalError(
            () => ReadHandlers.GetPlayerLocation("fixture-missing-location", "missing-player"),
            "missing getPlayerLocation"
        );
        AssertSingleTerminalPayload(
            () => ReadHandlers.GetPlayerInventory("fixture-missing-inventory", "missing-player"),
            new JArray(),
            "missing getPlayerInventory"
        );
    }

    private static void AssertSingleTerminalError(Action invoke, string description)
    {
        HandlerProbe.Configure(JValue.CreateNull());
        invoke();
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            description + " has exactly one terminal response"
        );
        WebSocketMessage terminal = WebSocketTransport.Instance.TerminalMessages[0];
        Equal(WebSocketMessage.MessageTypes.Error, terminal.Type, description + " is an error");
        JObject serialized = JObject.Parse(JsonConvert.SerializeObject(terminal));
        True(
            !string.IsNullOrEmpty((string)serialized["payload"]["error"]),
            description + " explains the failure"
        );
    }

    private static void AssertSingleTerminalPayload(
        Action invoke,
        JToken expectedPayload,
        string description
    )
    {
        HandlerProbe.Configure(JValue.CreateNull());
        invoke();
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            description + " has exactly one terminal response"
        );
        WebSocketMessage terminal = WebSocketTransport.Instance.TerminalMessages[0];
        Equal(
            WebSocketMessage.MessageTypes.Response,
            terminal.Type,
            description + " is a response"
        );
        JObject serialized = JObject.Parse(JsonConvert.SerializeObject(terminal));
        TokenEqual(expectedPayload, serialized["payload"], description + " payload semantics");
    }

    private static void AssertWorldDtoSerialization(JObject fixture)
    {
        var entity = new Takaro.TakaroEntity
        {
            Code = "zombieArlene",
            Name = "Arlene",
            Description = "Hostile zombie",
            Type = "hostile",
            Metadata = new Dictionary<string, object> { { "source", "7d2d" } },
        };
        JToken entityResponse = JToken.Parse(
            JsonConvert.SerializeObject(
                WebSocketMessage.CreateResponse("fixture-request", new[] { entity })
            )
        );
        TokenEqual(
            Function(fixture, "listEntities")["response"],
            entityResponse,
            "complete listEntities response fixture"
        );

        var location = new Takaro.TakaroLocation
        {
            Code = "army_camp_01@100,50,200:r1",
            Name = "Fort Camo",
            Position = new Takaro.TakaroPosition
            {
                X = 100,
                Y = 50,
                Z = 200,
            },
            SizeX = 61,
            SizeY = 28,
            SizeZ = 53,
            Metadata = new Dictionary<string, object>
            {
                { "prefab", "army_camp_01" },
                { "rotation", 1 },
                { "positionAnchor", "min-corner" },
            },
        };
        JToken locationResponse = JToken.Parse(
            JsonConvert.SerializeObject(
                WebSocketMessage.CreateResponse("fixture-request", new[] { location })
            )
        );
        JToken expectedLocationResponse = Function(fixture, "listLocations")["response"];
        TokenEqual(
            expectedLocationResponse,
            locationResponse,
            "complete rectangular listLocations response fixture"
        );
        JObject locationPayload = (JObject)locationResponse["payload"][0];
        True(locationPayload["position"] != null, "location has nested position");
        True(locationPayload["radius"] == null, "rectangular location omits radius");
        True(locationPayload["sizeX"] != null, "rectangular location has sizeX");
        True(locationPayload["sizeY"] != null, "rectangular location has sizeY");
        True(locationPayload["sizeZ"] != null, "rectangular location has sizeZ");
    }

    private static void AssertRouterParsingAndCardinality(JObject fixture)
    {
        JArray functions = (JArray)fixture["functions"];
        Equal(17, functions.Count, "fixture function count");

        var successRoutes = new HashSet<string>();
        foreach (JObject function in functions)
        {
            string action = (string)function["name"];
            foreach (JObject argumentCase in (JArray)function["argumentCases"])
            {
                string caseName = (string)argumentCase["name"];
                if (caseName != "object" && caseName != "json-string")
                    continue;

                JToken expectedPayload =
                    argumentCase["responsePayload"] ?? function["response"]["payload"];
                HandlerProbe.Configure(expectedPayload);

                var request = new JObject
                {
                    ["type"] = "request",
                    ["requestId"] = "fixture-request",
                    ["payload"] = new JObject
                    {
                        ["action"] = action,
                        ["args"] = argumentCase["value"].DeepClone(),
                    },
                };
                RequestRouter.Route(request.ToString(Formatting.None));

                Equal(
                    1,
                    WebSocketTransport.Instance.TerminalMessages.Count,
                    action + "/" + caseName + " has exactly one terminal response"
                );
                if (HandlerProbe.Actions.Count == 0)
                {
                    Equal(
                        0,
                        HandlerProbe.Actions.Count,
                        action + "/" + caseName + " bypasses the fake action-handler probe"
                    );
                    Equal(
                        WebSocketMessage.MessageTypes.Response,
                        WebSocketTransport.Instance.TerminalMessages[0].Type,
                        action + "/" + caseName + " reaches the production read handler"
                    );
                    Equal(
                        "fixture-request",
                        WebSocketTransport.Instance.TerminalMessages[0].RequestId,
                        action + "/" + caseName + " preserves request correlation"
                    );
                    if (((JObject)function["args"]).Count > 0)
                    {
                        JObject terminal = JObject.Parse(
                            JsonConvert.SerializeObject(
                                WebSocketTransport.Instance.TerminalMessages[0]
                            )
                        );
                        True(
                            terminal["payload"].Type != JTokenType.Null,
                            action + "/" + caseName + " passes the fixture player argument"
                        );
                    }
                    successRoutes.Add(action);
                    continue;
                }

                Equal(1, HandlerProbe.Actions.Count, action + "/" + caseName + " delegates once");
                Equal(
                    action,
                    HandlerProbe.Actions[0],
                    action + "/" + caseName + " delegates correctly"
                );
                if (
                    ((JObject)function["args"]).Count > 0
                    && (caseName == "object" || caseName == "json-string")
                )
                {
                    True(
                        HandlerProbe.ParsedArguments != null,
                        action + "/" + caseName + " reaches the handler with parsed arguments"
                    );
                }
                Equal(
                    WebSocketMessage.MessageTypes.Response,
                    WebSocketTransport.Instance.TerminalMessages[0].Type,
                    action + "/" + caseName + " returns one response at the router seam"
                );

                if (caseName == "object" || caseName == "json-string")
                    successRoutes.Add(action);
            }
        }
        Equal(17, successRoutes.Count, "all 17 valid function routes exercise the router seam");

        foreach (string action in new[] { "getPlayer", "getPlayerLocation", "getPlayerInventory" })
        {
            HandlerProbe.Configure(JValue.CreateNull());
            var invalidRequest = new JObject
            {
                ["type"] = "request",
                ["requestId"] = "fixture-request",
                ["payload"] = new JObject { ["action"] = action, ["args"] = new JObject() },
            };
            RequestRouter.Route(invalidRequest.ToString(Formatting.None));
            Equal(0, HandlerProbe.Actions.Count, action + " rejects a missing gameId");
            Equal(
                1,
                WebSocketTransport.Instance.TerminalMessages.Count,
                action + " emits exactly one terminal error"
            );
            Equal(
                WebSocketMessage.MessageTypes.Error,
                WebSocketTransport.Instance.TerminalMessages[0].Type,
                action + " emits an error response"
            );
        }
    }

    private static void AssertControlFramesDoNotEnterRequestDispatch()
    {
        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            "{\"type\":\"pong\",\"requestId\":\"fixture-heartbeat\",\"payload\":{\"timestamp\":\"2030-01-01T00:00:00Z\"}}"
        );
        Equal(0, HandlerProbe.Actions.Count, "pong does not dispatch an action");
        Equal(
            0,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "pong does not emit a terminal response"
        );

        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            "{\"type\":\"request\",\"requestId\":\"fixture-malformed\",\"payload\":{}}"
        );
        Equal(0, HandlerProbe.Actions.Count, "malformed request does not dispatch an action");
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "malformed request emits exactly one terminal response"
        );
        Equal(
            WebSocketMessage.MessageTypes.Error,
            WebSocketTransport.Instance.TerminalMessages[0].Type,
            "malformed request emits an error response"
        );

        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            "{\"type\":\"request\",\"requestId\":\"fixture-valid\",\"payload\":{\"action\":\"testReachability\",\"args\":{}}}"
        );
        Equal(0, HandlerProbe.Actions.Count, "valid read request uses the production handler");
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "valid request emits exactly one terminal response"
        );
        Equal(
            WebSocketMessage.MessageTypes.Response,
            WebSocketTransport.Instance.TerminalMessages[0].Type,
            "valid request emits a response"
        );
    }

    private static void AssertProtocolErrorsAreBoundedAndSafe()
    {
        const string secret = "fixture-registration-secret";
        LogService.Instance.Messages.Clear();
        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            "{\"type\":\"error\",\"requestId\":\"fixture-error\",\"payload\":{\"message\":\"first\\nsecond\",\"registrationToken\":\""
                + secret
                + "\"}}"
        );

        Equal(0, HandlerProbe.Actions.Count, "protocol error does not dispatch an action");
        Equal(
            0,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "protocol error does not emit a terminal response"
        );
        Equal(1, LogService.Instance.Messages.Count, "protocol error emits one diagnostic");
        string diagnostic = LogService.Instance.Messages[0];
        True(diagnostic.Contains("fixture-error"), "protocol diagnostic preserves correlation");
        True(diagnostic.Contains("first second"), "protocol diagnostic normalizes newlines");
        True(!diagnostic.Contains(secret), "protocol diagnostic omits token value");
        True(!diagnostic.Contains("registrationToken"), "protocol diagnostic omits payload fields");

        LogService.Instance.Messages.Clear();
        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            JsonConvert.SerializeObject(
                new { type = "error", payload = new { message = new string('x', 2048) } }
            )
        );
        Equal(1, LogService.Instance.Messages.Count, "uncorrelated protocol error is logged");
        True(
            LogService.Instance.Messages[0].Length <= ProtocolDiagnostics.MaxMessageLength + 64,
            "protocol diagnostic is bounded"
        );
    }

    private static void AssertRawRequestsAreNotLogged()
    {
        const string secret = "fixture-registration-secret";
        LogService.Instance.Messages.Clear();
        HandlerProbe.Configure(JValue.CreateNull());
        RequestRouter.Route(
            "{\"type\":\"request\",\"requestId\":\"fixture-request\",\"payload\":{\"action\":\"testReachability\",\"args\":\"{\\\"registrationToken\\\":\\\""
                + secret
                + "\\\"}\"}}"
        );
        foreach (string line in LogService.Instance.Messages)
        {
            True(!line.Contains(secret), "request log omits token value");
            True(!line.Contains("registrationToken"), "request log omits raw args");
        }
        True(LogService.Instance.Messages.Count > 0, "request emits metadata-only log");
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "metadata-only request has one terminal response"
        );
    }

    private static void AssertCorrelatedMalformedRequestsTerminate()
    {
        foreach (
            string request in new[]
            {
                "{\"type\":\"request\",\"requestId\":\"fixture-request\",\"payload\":null}",
                "{\"type\":\"request\",\"requestId\":\"fixture-request\",\"payload\":\"invalid\"}",
                "{\"type\":\"request\",\"requestId\":\"fixture-request\",\"payload\":{}}",
                "{\"type\":\"request\",\"requestId\":\"fixture-request\",\"payload\":{\"action\":\"\"}}",
            }
        )
        {
            HandlerProbe.Configure(JValue.CreateNull());
            RequestRouter.Route(request);
            Equal(
                1,
                WebSocketTransport.Instance.TerminalMessages.Count,
                "correlated malformed request has one terminal response"
            );
            Equal(
                WebSocketMessage.MessageTypes.Error,
                WebSocketTransport.Instance.TerminalMessages[0].Type,
                "correlated malformed request returns an error"
            );
        }
    }

    /// <summary>
    /// getMapInfo must always satisfy Takaro's MapInfoDTO. The shipped 0.1.3
    /// build had no handler at all, so Takaro validated the router's error reply
    /// and failed on "property enabled has failed ... isBoolean".
    /// </summary>
    private static void AssertMapCatalog()
    {
        string root = Path.Combine(
            Path.GetTempPath(),
            "takaro-map-" + Guid.NewGuid().ToString("N")
        );

        // No tile cache at all (web dashboard disabled, the common case).
        Dictionary<string, object> disabled = MapCatalog.BuildMapInfo(
            Path.Combine(root, "missing"),
            6144,
            256,
            6144
        );
        AssertMapInfoShape(disabled, "map info without a tile cache");
        Equal(false, (bool)disabled["enabled"], "missing tile cache reports enabled=false");
        Equal(
            MapCatalog.FallbackMaxZoom,
            (int)disabled["maxZoom"],
            "missing tile cache still reports a usable maxZoom"
        );
        Equal(6144, (int)disabled["mapSizeX"], "map info echoes the world extent X");
        Equal(256, (int)disabled["mapSizeY"], "map info echoes the world extent Y");
        Equal(6144, (int)disabled["mapSizeZ"], "map info echoes the world extent Z");
        Equal(
            MapCatalog.TileBlockSize,
            (int)disabled["mapBlockSize"],
            "map info reports the tile block size"
        );

        Dictionary<string, object> nullRoot = MapCatalog.BuildMapInfo(null, -5, -5, -5);
        AssertMapInfoShape(nullRoot, "map info with an unresolved save directory");
        Equal(false, (bool)nullRoot["enabled"], "unresolved save dir reports enabled=false");
        Equal(0, (int)nullRoot["mapSizeX"], "negative world extent is clamped to zero");

        // A real tile cache.
        string mapRoot = Path.Combine(root, "map");
        Directory.CreateDirectory(Path.Combine(mapRoot, "3", "-2"));
        Directory.CreateDirectory(Path.Combine(mapRoot, "notazoom"));
        byte[] png = new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x01 };
        File.WriteAllBytes(Path.Combine(mapRoot, "3", "-2", "7.png"), png);

        try
        {
            Dictionary<string, object> enabled = MapCatalog.BuildMapInfo(mapRoot, 8192, 256, 8192);
            AssertMapInfoShape(enabled, "map info with a tile cache");
            Equal(true, (bool)enabled["enabled"], "present tile cache reports enabled=true");
            Equal(3, (int)enabled["maxZoom"], "maxZoom is the highest numeric zoom directory");

            int discovered;
            True(
                MapCatalog.TryInspect(mapRoot, out discovered),
                "tile cache with numeric zoom directories is usable"
            );
            Equal(3, discovered, "non-numeric zoom directories are ignored");

            string tile = MapCatalog.TryReadTileBase64(mapRoot, 3, -2, 7);
            Equal(
                Convert.ToBase64String(png),
                tile,
                "an existing tile is returned as base64 PNG bytes"
            );
            True(
                MapCatalog.TryReadTileBase64(mapRoot, 3, -2, 8) == null,
                "an unrendered tile reads as null rather than throwing"
            );
            True(
                MapCatalog.TryReadTileBase64(null, 3, -2, 7) == null,
                "an unresolved tile root reads as null rather than throwing"
            );
            True(
                MapCatalog
                    .ResolveTilePath(mapRoot, 3, -2, 7)
                    .EndsWith(Path.Combine("3", "-2", "7.png")),
                "tile paths follow the <zoom>/<x>/<y>.png layout"
            );
        }
        finally
        {
            try
            {
                Directory.Delete(root, true);
            }
            catch (Exception) { }
        }
    }

    private static void AssertMapInfoShape(Dictionary<string, object> info, string description)
    {
        JObject json = JObject.Parse(JsonConvert.SerializeObject(info));
        True(json["enabled"] != null, description + ": enabled is present");
        Equal(
            JTokenType.Boolean,
            json["enabled"].Type,
            description + ": enabled serializes as a JSON boolean"
        );
        foreach (
            string numeric in new[]
            {
                "mapBlockSize",
                "maxZoom",
                "mapSizeX",
                "mapSizeY",
                "mapSizeZ",
            }
        )
        {
            True(json[numeric] != null, description + ": " + numeric + " is present");
            Equal(
                JTokenType.Integer,
                json[numeric].Type,
                description + ": " + numeric + " serializes as a JSON number"
            );
        }
    }

    /// <summary>
    /// The router must dispatch getMapInfo/getMapTile instead of falling through
    /// to "Unknown message type".
    /// </summary>
    private static void AssertMapRouting()
    {
        Takaro.Services.StateMirror.Instance.MapRoot = null;
        Takaro.Services.StateMirror.Instance.MapSizeX = 6144;
        Takaro.Services.StateMirror.Instance.MapSizeY = 256;
        Takaro.Services.StateMirror.Instance.MapSizeZ = 6144;

        WebSocketTransport.Instance.TerminalMessages.Clear();
        HandlerProbe.Configure(JValue.CreateNull());
        RouteMapRequest("getMapInfo", "map-info-1", null);
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "getMapInfo has exactly one terminal response"
        );
        WebSocketMessage mapInfoMessage = WebSocketTransport.Instance.TerminalMessages[0];
        Equal(
            WebSocketMessage.MessageTypes.Response,
            mapInfoMessage.Type,
            "getMapInfo is routed to a response, not Unknown message type"
        );
        Equal("map-info-1", mapInfoMessage.RequestId, "getMapInfo preserves request correlation");
        JObject info = (JObject)
            JObject.Parse(JsonConvert.SerializeObject(mapInfoMessage))["payload"];
        Equal(
            JTokenType.Boolean,
            info["enabled"].Type,
            "routed getMapInfo returns a boolean enabled"
        );
        Equal(false, info["enabled"].Value<bool>(), "routed getMapInfo reports disabled");
        Equal(
            6144,
            info["mapSizeX"].Value<int>(),
            "routed getMapInfo carries the captured world extent"
        );
        Equal(
            0,
            HandlerProbe.Actions.Count,
            "getMapInfo is answered by the production read handler"
        );

        WebSocketTransport.Instance.TerminalMessages.Clear();
        HandlerProbe.Configure(JValue.CreateNull());
        RouteMapRequest(
            "getMapTile",
            "map-tile-1",
            new JObject
            {
                ["x"] = 1,
                ["y"] = 2,
                ["z"] = 3,
            }
        );
        Equal(
            1,
            WebSocketTransport.Instance.TerminalMessages.Count,
            "getMapTile has exactly one terminal response"
        );
        WebSocketMessage tileMessage = WebSocketTransport.Instance.TerminalMessages[0];
        Equal(
            WebSocketMessage.MessageTypes.Error,
            tileMessage.Type,
            "getMapTile without a tile cache fails explicitly"
        );
        string tileError = (string)
            JObject.Parse(JsonConvert.SerializeObject(tileMessage))["payload"]["error"];
        True(
            !string.IsNullOrEmpty(tileError) && tileError.IndexOf("Unknown message type") < 0,
            "getMapTile error explains the missing tile cache instead of an unknown action"
        );

        WebSocketTransport.Instance.TerminalMessages.Clear();
    }

    private static void RouteMapRequest(string action, string requestId, JToken args)
    {
        var payload = new JObject { ["action"] = action };
        if (args != null)
            payload["args"] = args;

        var request = new JObject
        {
            ["type"] = "request",
            ["requestId"] = requestId,
            ["payload"] = payload,
        };
        RequestRouter.Route(request.ToString(Formatting.None));
    }

    private static void AssertConfigFiles()
    {
        string shippedPath = "/app/mod/Config.xml";
        var defaults = new ConfigValues
        {
            Url = ConfigFiles.DefaultUrl,
            RegistrationToken = "",
            IdentityToken = "",
            Enabled = true,
            ReconnectIntervalSeconds = ConfigFiles.DefaultReconnectIntervalSeconds,
        };
        Equal(
            ConfigFiles.Render(defaults, ConfigFiles.ModConfigHeader),
            File.ReadAllText(shippedPath),
            "shipped Config.xml is what the mod writes for a fresh install"
        );
        ConfigValues shipped = ConfigFiles.Read(shippedPath);
        Equal("", shipped.RegistrationToken, "shipped config has no registration token");
        Equal("", shipped.IdentityToken, "shipped config has no identity token");

        int generated = 0;
        Func<string> newIdentity = () => "generated-" + ++generated;

        ConfigValues fresh = ConfigFiles.Resolve(shipped, null, newIdentity, out bool made);
        True(made && fresh.IdentityToken == "generated-1", "fresh install generates an identity");
        Equal("", fresh.RegistrationToken, "fresh install waits for a registration token");
        Equal(ConfigFiles.DefaultUrl, fresh.Url, "fresh install uses the production endpoint");

        var saved = new ConfigValues
        {
            Url = "wss://connect.takaro.io/",
            RegistrationToken = "saved-reg",
            IdentityToken = "saved-id",
            Enabled = true,
            ReconnectIntervalSeconds = 30,
        };
        ConfigValues upgraded = ConfigFiles.Resolve(shipped, saved, newIdentity, out made);
        True(!made, "an upgrade keeps the saved identity");
        Equal("saved-reg", upgraded.RegistrationToken, "an upgrade keeps the saved token");
        Equal("saved-id", upgraded.IdentityToken, "an upgrade keeps the saved identity");

        var edited = new ConfigValues { RegistrationToken = "new-reg", IdentityToken = "" };
        ConfigValues fixedToken = ConfigFiles.Resolve(edited, saved, newIdentity, out made);
        Equal("new-reg", fixedToken.RegistrationToken, "a token in the mod config wins");
        Equal("saved-id", fixedToken.IdentityToken, "a new token keeps the saved identity");
        True(!fixedToken.SameConnection(upgraded), "a changed token is a connection change");
        True(
            upgraded.SameConnection(ConfigFiles.Resolve(shipped, saved, newIdentity, out _)),
            "re-reading unchanged files is not a connection change"
        );

        var customised = new ConfigValues
        {
            Url = "wss://takaro.example.org/",
            RegistrationToken = "saved-reg",
            IdentityToken = "saved-id",
            Enabled = false,
            ReconnectIntervalSeconds = 60,
        };
        ConfigValues keptCustom = ConfigFiles.Resolve(shipped, customised, newIdentity, out _);
        True(
            keptCustom.SameAs(customised),
            "the shipped defaults do not override saved settings on an upgrade"
        );
        var modCustom = new ConfigValues
        {
            Url = "wss://other.example.org/",
            Enabled = false,
            ReconnectIntervalSeconds = 90,
        };
        ConfigValues modWins = ConfigFiles.Resolve(modCustom, saved, newIdentity, out _);
        True(
            modWins.Url == "wss://other.example.org/"
                && modWins.Enabled == false
                && modWins.ReconnectIntervalSeconds == 90,
            "a setting changed in the mod config wins"
        );

        ConfigValues copy = ConfigFiles.SavedCopy(modWins, customised);
        True(
            copy.Url == customised.Url
                && copy.Enabled == customised.Enabled
                && copy.ReconnectIntervalSeconds == customised.ReconnectIntervalSeconds
                && copy.RegistrationToken == modWins.RegistrationToken
                && copy.IdentityToken == modWins.IdentityToken,
            "the saved copy takes the tokens in use and keeps its own other settings"
        );
        ConfigValues firstCopy = ConfigFiles.SavedCopy(modWins, null);
        True(
            firstCopy.Url == ConfigFiles.DefaultUrl
                && firstCopy.Enabled == true
                && firstCopy.ReconnectIntervalSeconds
                    == ConfigFiles.DefaultReconnectIntervalSeconds,
            "a new saved copy does not freeze mod-config settings"
        );
        True(
            ConfigFiles.SavedCopy(upgraded, saved).SameAs(saved),
            "an unchanged install does not rewrite its saved config"
        );

        ConfigValues legacyOnly = ConfigFiles.Resolve(null, saved, newIdentity, out made);
        True(!made && legacyOnly.SameAs(saved), "a legacy-only install uses its old config");

        var placeholder = new ConfigValues
        {
            RegistrationToken = "reg",
            IdentityToken = ConfigFiles.PlaceholderIdentityToken,
        };
        ConfigFiles.Resolve(placeholder, null, newIdentity, out made);
        True(made, "the old placeholder identity is replaced");

        string dir = Path.Combine(Path.GetTempPath(), "takaro-config-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        try
        {
            string path = Path.Combine(dir, "Config.xml");
            File.WriteAllText(
                path,
                File.ReadAllText(shippedPath)
                    .Replace(
                        "<RegistrationToken></RegistrationToken>",
                        "<RegistrationToken>\n   pasted-token  \n</RegistrationToken>"
                    )
            );
            Equal(
                "pasted-token",
                ConfigFiles.Read(path).RegistrationToken,
                "a pasted token is trimmed"
            );

            ConfigFiles.WriteIdentity(path, "written-id");
            string text = File.ReadAllText(path);
            True(
                text.Contains("Paste the registration token"),
                "writing the identity keeps the comments"
            );
            ConfigValues reread = ConfigFiles.Read(path);
            Equal("written-id", reread.IdentityToken, "identity is written");
            Equal("pasted-token", reread.RegistrationToken, "writing the identity keeps the token");
            True(!File.Exists(path + ".tmp"), "no temporary file is left behind");

            string savedPath = Path.Combine(dir, "Saved.xml");
            ConfigFiles.WriteAll(savedPath, saved, ConfigFiles.SavedConfigHeader);
            True(ConfigFiles.Read(savedPath).SameAs(saved), "the saved copy round-trips");
            ConfigFiles.WriteAll(savedPath, upgraded, ConfigFiles.SavedConfigHeader);
            True(ConfigFiles.Read(savedPath).SameAs(upgraded), "the saved copy can be replaced");

            File.WriteAllText(path, "<Takaro><WebSocket><RegistrationToken>half");
            bool threw = false;
            try
            {
                ConfigFiles.Read(path);
            }
            catch (System.Xml.XmlException)
            {
                threw = true;
            }
            True(threw, "a half-saved file is reported, not read as empty");
            True(
                ConfigFiles.Read(Path.Combine(dir, "missing.xml")) == null,
                "a missing file reads as null"
            );
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    private static void True(bool condition, string description)
    {
        _assertions++;
        if (!condition)
            throw new Exception("Assertion failed: " + description);
    }

    private static void Equal<T>(T expected, T actual, string description)
    {
        _assertions++;
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new Exception(
                "Assertion failed: " + description + "; expected " + expected + ", got " + actual
            );
    }

    private static JObject Function(JObject fixture, string name)
    {
        foreach (JObject function in (JArray)fixture["functions"])
        {
            if ((string)function["name"] == name)
                return function;
        }
        throw new Exception("Missing fixture function " + name);
    }

    private static void TokenEqual(JToken expected, JToken actual, string description)
    {
        _assertions++;
        if (!JToken.DeepEquals(expected, actual))
            throw new Exception(
                "Assertion failed: " + description + "; expected " + expected + ", got " + actual
            );
    }
}

public static class HandlerProbe
{
    public static readonly List<string> Actions = new List<string>();
    public static object ParsedArguments { get; private set; }
    private static JToken _responsePayload;

    public static void Configure(JToken responsePayload)
    {
        Actions.Clear();
        ParsedArguments = null;
        _responsePayload = responsePayload.DeepClone();
        WebSocketTransport.Instance.TerminalMessages.Clear();
    }

    public static void Complete(string action, string requestId, object parsedArgs = null)
    {
        Actions.Add(action);
        ParsedArguments = parsedArgs;
        WebSocketTransport.Instance.Send(
            WebSocketMessage.CreateResponse(requestId, _responsePayload.ToObject<object>())
        );
    }
}

namespace Takaro.WebSocket
{
    public static class ActionHandlers
    {
        private static Task Done(string action, string requestId, object args = null)
        {
            HandlerProbe.Complete(action, requestId, args);
            return Task.CompletedTask;
        }

        public static Task GiveItem(string requestId, TakaroGiveItemArgs args)
        {
            return Done("giveItem", requestId, args);
        }

        public static Task ExecuteCommand(string requestId, TakaroExecuteCommandArgs args)
        {
            return Done("executeConsoleCommand", requestId, args);
        }

        public static Task SendChatMessage(string requestId, TakaroSendMessageArgs args)
        {
            return Done("sendMessage", requestId, args);
        }

        public static Task KickPlayer(string requestId, TakaroKickPlayerArgs args)
        {
            return Done("kickPlayer", requestId, args);
        }

        public static Task BanPlayer(string requestId, TakaroBanPlayerArgs args)
        {
            return Done("banPlayer", requestId, args);
        }

        public static Task UnbanPlayer(string requestId, TakaroUnbanPlayerArgs args)
        {
            return Done("unbanPlayer", requestId, args);
        }

        public static Task TeleportPlayer(string requestId, TakaroTeleportPlayerArgs args)
        {
            return Done("teleportPlayer", requestId, args);
        }

        public static Task Shutdown(string requestId)
        {
            return Done("shutdown", requestId);
        }
    }

    public sealed class WebSocketTransport
    {
        public static readonly WebSocketTransport Instance = new WebSocketTransport();
        public readonly List<WebSocketMessage> TerminalMessages = new List<WebSocketMessage>();

        public void Send(WebSocketMessage message)
        {
            TerminalMessages.Add(message);
        }

        public void SendErrorResponse(string requestId, string message)
        {
            Send(WebSocketMessage.CreateErrorResponse(requestId, message));
        }
    }
}

namespace Takaro.Services
{
    public sealed class MainThreadDispatcher
    {
        public static readonly MainThreadDispatcher Instance = new MainThreadDispatcher();

        public Task Run(Action fn)
        {
            fn();
            return Task.CompletedTask;
        }
    }

    public sealed class StateMirror
    {
        public static readonly StateMirror Instance = new StateMirror();

        public bool IsGameReady => true;

        public string MapRoot;
        public int MapSizeX;
        public int MapSizeY;
        public int MapSizeZ;

        public List<Takaro.TakaroPlayer> GetOnlinePlayers()
        {
            return new List<Takaro.TakaroPlayer>
            {
                Takaro.Shared.TransformPlayerRecordToTakaroPlayer(FixturePlayer()),
            };
        }

        public Takaro.Persistence.PlayerRecord GetOnlinePlayer(string gameId)
        {
            return gameId == "fixture-player" ? FixturePlayer() : null;
        }

        public Takaro.Persistence.PlayerRecord GetPlayerLocationRecord(string gameId)
        {
            return gameId == "fixture-player" ? FixturePlayer() : null;
        }

        public List<Takaro.TakaroItem> GetPlayerInventory(string gameId)
        {
            return new List<Takaro.TakaroItem>
            {
                new Takaro.TakaroItem
                {
                    Code = "resourceWood",
                    Name = "Wood",
                    Amount = 2,
                    Quality = "1",
                },
            };
        }

        public List<Takaro.TakaroItem> GetItems()
        {
            return GetPlayerInventory("fixture-player");
        }

        public List<Takaro.TakaroEntity> GetEntities()
        {
            return new List<Takaro.TakaroEntity>();
        }

        public List<Takaro.TakaroLocation> GetLocations()
        {
            return new List<Takaro.TakaroLocation>();
        }

        public List<Takaro.TakaroBan> GetBans()
        {
            return new List<Takaro.TakaroBan>();
        }

        private static Takaro.Persistence.PlayerRecord FixturePlayer()
        {
            return new Takaro.Persistence.PlayerRecord
            {
                GameId = "fixture-player",
                Name = "Fixture Player",
                Ping = 42,
                SteamId = "fixture-player",
                EpicOnlineServicesId = "fixture-player",
                X = 10.5f,
                Y = 20.25f,
                Z = 30.75f,
            };
        }
    }

    public sealed class LogService
    {
        public static readonly LogService Instance = new LogService();
        public readonly List<string> Messages = new List<string>();

        public void Debug(string message)
        {
            Messages.Add(message);
        }

        public void Warn(string message)
        {
            Messages.Add(message);
        }

        public void Error(string message)
        {
            Messages.Add(message);
        }
    }
}

public static class Log
{
    public static void Exception(Exception ex) { }
}

namespace Takaro.Persistence
{
    public sealed class PlayerRecord
    {
        public string GameId { get; set; }
        public string Name { get; set; }
        public string Ip { get; set; }
        public int Ping { get; set; }
        public string SteamId { get; set; }
        public string XboxLiveId { get; set; }
        public string EpicOnlineServicesId { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
        public bool Online { get; set; }
        public DateTime LastSeenUtc { get; set; }
    }

    public sealed class BanRecord
    {
        public string GameId { get; set; }
        public string Name { get; set; }
        public string SteamId { get; set; }
        public string XboxLiveId { get; set; }
        public string EpicOnlineServicesId { get; set; }
        public string Reason { get; set; }
        public string ExpiresAt { get; set; }
    }
}

public sealed class PlatformUserIdentifierAbs
{
    public string CombinedString { get; set; }

    public static PlatformUserIdentifierAbs FromCombinedString(string value)
    {
        return new PlatformUserIdentifierAbs { CombinedString = value };
    }
}

public sealed class ClientInfo
{
    public PlatformUserIdentifierAbs CrossplatformId { get; set; }
    public PlatformUserIdentifierAbs PlatformId { get; set; }
    public string playerName { get; set; }
    public string ip { get; set; }
    public int ping { get; set; }
    public int entityId { get; set; }
    public readonly List<NetPackage> SentPackages = new List<NetPackage>();

    public void SendPackage(NetPackage package)
    {
        SentPackages.Add(package);
    }
}

public enum EChatType
{
    Global,
    Whisper,
    Friends,
    Party,
}

public sealed class ConnectionManager
{
    public static readonly ConnectionManager Instance = new ConnectionManager();
    public readonly ClientCollection Clients = new ClientCollection();
}

public sealed class ClientCollection
{
    public ClientInfo FixtureClient { get; set; }

    public ClientInfo ForUserId(PlatformUserIdentifierAbs userId)
    {
        return userId != null && userId.CombinedString == "EOS_fixture-player"
            ? FixtureClient
            : null;
    }
}

public sealed class ItemClass
{
    public static readonly Dictionary<int, ItemClass> list = new Dictionary<int, ItemClass>();
    public bool HasSubItems { get; set; }
    public bool HasQuality { get; set; }

    public static ItemValue GetItem(string code)
    {
        return code == "resourceWood" ? new ItemValue(42, true) : ItemValue.None;
    }

    public string GetItemName()
    {
        return "fixture";
    }

    public string GetLocalizedItemName()
    {
        return "Fixture";
    }
}

public sealed class ItemValue
{
    public int type;
    public ushort Quality;
    public ItemValue[] Modifications;
    public static readonly ItemValue None = new ItemValue(-1, false);

    public ItemValue(int type, bool useQuality)
    {
        this.type = type;
        Quality = 0;
        Modifications = new ItemValue[0];
    }

    // The V3.3.0 accessors that replaced the public Modifications array.
    public int ModificationCount => Modifications.Length;

    public ItemValue GetModification(int index) => Modifications[index];

    public void SetModification(int index, ItemValue mod) => Modifications[index] = mod;
}

public sealed class ItemStack
{
    public readonly ItemValue itemValue;
    public readonly int count;

    public ItemStack(ItemValue itemValue, int count)
    {
        this.itemValue = itemValue;
        this.count = count;
    }
}

public sealed class EntityPlayer
{
    private readonly UnityEngine.Vector3 _dropPosition;
    public readonly int entityId;
    public bool Spawned { get; set; } = true;
    public bool Dead { get; set; }

    public EntityPlayer(int entityId, UnityEngine.Vector3 dropPosition)
    {
        this.entityId = entityId;
        _dropPosition = dropPosition;
    }

    public UnityEngine.Vector3 GetDropPosition()
    {
        return _dropPosition;
    }

    public bool IsSpawned()
    {
        return Spawned;
    }

    public bool IsDead()
    {
        return Dead;
    }
}

public sealed class EntityPlayerCollection
{
    public readonly Dictionary<int, EntityPlayer> dict = new Dictionary<int, EntityPlayer>();
}

public sealed class World
{
    public readonly EntityPlayerCollection Players = new EntityPlayerCollection();
    public readonly List<EntityItem> Spawned = new List<EntityItem>();
    public readonly List<int> Removed = new List<int>();

    public void SpawnEntityInWorld(EntityItem entity)
    {
        Spawned.Add(entity);
    }

    public void RemoveEntity(int entityId, EnumRemoveEntityReason reason)
    {
        Removed.Add(entityId);
    }
}

public enum EnumRemoveEntityReason
{
    Killed,
}

public sealed class EntityCreationData
{
    public int entityClass;
    public int id;
    public ItemStack itemStack;
    public UnityEngine.Vector3 pos;
    public UnityEngine.Vector3 rot;
    public float lifetime;
    public int belongsPlayerId;
}

public class Entity
{
    public int entityId;
}

public sealed class EntityItem : Entity
{
    public EntityCreationData CreationData;
}

public static class EntityClass
{
    public static int FromString(string name)
    {
        return name == "item" ? 1 : -1;
    }
}

public static class EntityFactory
{
    public static int nextEntityID = 1000;

    public static Entity CreateEntity(EntityCreationData data)
    {
        return new EntityItem { entityId = data.id, CreationData = data };
    }
}

public abstract class NetPackage { }

public sealed class NetPackageEntityCollect : NetPackage
{
    public int EntityId;
    public int PlayerId;

    public NetPackageEntityCollect Setup(int entityId, int playerId)
    {
        EntityId = entityId;
        PlayerId = playerId;
        return this;
    }
}

public static class NetPackageManager
{
    public static T GetPackage<T>()
        where T : NetPackage, new()
    {
        return new T();
    }
}

public sealed class ItemDropCall
{
    public ItemStack Stack;
    public UnityEngine.Vector3 Position;
    public UnityEngine.Vector3 RandomPosition;
    public int BelongsPlayerId;
    public float Lifetime;
    public bool RelativeToHead;
}

public sealed class GameManager
{
    public static readonly GameManager Instance = new GameManager();
    public readonly List<ItemDropCall> ItemDrops = new List<ItemDropCall>();
    public readonly World World = new World();

    public void ResetItemDrops()
    {
        ItemDrops.Clear();
        World.Spawned.Clear();
        World.Removed.Clear();
    }

    public void ResetGiveItemFixture()
    {
        ResetItemDrops();
        World.Players.dict.Clear();
        ItemClass.list.Clear();
        var player = new EntityPlayer(73, new UnityEngine.Vector3(10.5f, 20.25f, 30.75f));
        World.Players.dict[player.entityId] = player;
        ConnectionManager.Instance.Clients.FixtureClient = new ClientInfo
        {
            CrossplatformId = new PlatformUserIdentifierAbs
            {
                CombinedString = "EOS_fixture-player",
            },
            PlatformId = new PlatformUserIdentifierAbs { CombinedString = "Steam_fixture-player" },
            entityId = player.entityId,
            playerName = "Fixture Player",
        };
        ItemClass.list[42] = new ItemClass { HasQuality = true };
    }

    public void ItemDropServer(
        ItemStack itemStack,
        UnityEngine.Vector3 dropPosition,
        UnityEngine.Vector3 randomPosition,
        int belongsPlayerId = -1,
        float lifetime = 60f,
        bool dropPositionIsRelativeToHead = false
    )
    {
        ItemDrops.Add(
            new ItemDropCall
            {
                Stack = itemStack,
                Position = dropPosition,
                RandomPosition = randomPosition,
                BelongsPlayerId = belongsPlayerId,
                Lifetime = lifetime,
                RelativeToHead = dropPositionIsRelativeToHead,
            }
        );
    }
}

public static class Constants
{
    public const ushort cItemMaxQuality = 6;
}

public static class Localization
{
    public static string Get(string key, bool fallback)
    {
        return key;
    }
}

namespace UnityEngine
{
    public struct Vector3
    {
        public float x;
        public float y;
        public float z;

        public Vector3(float x, float y, float z)
        {
            this.x = x;
            this.y = y;
            this.z = z;
        }

        public static Vector3 zero => new Vector3(0f, 0f, 0f);
    }
}

public struct Vector3i
{
    public int x;
    public int y;
    public int z;

    public Vector3i(UnityEngine.Vector3 value)
    {
        x = (int)Math.Round(value.x);
        y = (int)Math.Round(value.y);
        z = (int)Math.Round(value.z);
    }
}
