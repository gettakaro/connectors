using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
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
            AssertResponseSerialization(fixture);
            AssertWorldDtoSerialization(fixture);
            AssertPlayerProximateItemDelivery();
            AssertGiveItemProductionValidationAndCardinality();
            AssertProductionNotFoundReadSemantics();
            AssertNestedArgumentParsing();
            AssertRouterParsingAndCardinality(fixture);
            AssertControlFramesDoNotEnterRequestDispatch();
            AssertCorrelatedMalformedRequestsTerminate();
            AssertRawRequestsAreNotLogged();
            Console.WriteLine("Contract harness passed: " + _assertions + " assertions");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    private static void AssertPlayerProximateItemDelivery()
    {
        GameManager.Instance.ResetItemDrops();
        var itemValue = new ItemValue(42, true) { Quality = 3 };
        var player = new EntityPlayer(73, new UnityEngine.Vector3(10.5f, 20.25f, 30.75f));

        PlayerProximateItemDelivery.Drop(itemValue, 7, player);

        Equal(1, GameManager.Instance.ItemDrops.Count, "delivery issues exactly one world drop");
        ItemDropCall drop = GameManager.Instance.ItemDrops[0];
        Equal(42, drop.Stack.itemValue.type, "delivery preserves item type");
        Equal((ushort)3, drop.Stack.itemValue.Quality, "delivery preserves item quality");
        Equal(7, drop.Stack.count, "delivery creates one stack with the requested amount");
        Equal(10.5f, drop.Position.x, "delivery uses target player x");
        Equal(20.25f, drop.Position.y, "delivery uses target player y");
        Equal(30.75f, drop.Position.z, "delivery uses target player z");
        Equal(0f, drop.RandomPosition.x, "delivery disables random x offset");
        Equal(0f, drop.RandomPosition.y, "delivery disables random y offset");
        Equal(0f, drop.RandomPosition.z, "delivery disables random z offset");
        Equal(
            -1,
            drop.BelongsPlayerId,
            "delivery matches the first-party remote give path without an owning entity"
        );
        Equal(60f, drop.Lifetime, "delivery preserves the first-party lifetime");
        Equal(false, drop.RelativeToHead, "delivery uses the resolved world position");
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
        Equal(1, GameManager.Instance.ItemDrops.Count, "valid giveItem creates one drop");
        Equal(2, GameManager.Instance.ItemDrops[0].Stack.count, "valid giveItem preserves amount");
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
                EpicOnlineServicesId = "fixture-eos",
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
                EpicOnlineServicesId = "fixture-eos",
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
        AssertSingleTerminalPayload(
            () => ReadHandlers.GetPlayer("fixture-missing-player", "missing-player"),
            JValue.CreateNull(),
            "missing getPlayer"
        );
        AssertSingleTerminalPayload(
            () => ReadHandlers.GetPlayerLocation("fixture-missing-location", "missing-player"),
            JValue.CreateNull(),
            "missing getPlayerLocation"
        );
        AssertSingleTerminalPayload(
            () => ReadHandlers.GetPlayerInventory("fixture-missing-inventory", "missing-player"),
            new JArray(),
            "missing getPlayerInventory"
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
