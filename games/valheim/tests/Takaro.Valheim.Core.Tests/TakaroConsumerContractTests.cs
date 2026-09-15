using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class TakaroConsumerContractTests
{
    private static readonly ConsumerActionContract[] PinnedValidationMap =
    {
        new("getPlayer", ConsumerPayloadShape.Object, "gameId", "name"),
        new("getPlayers", ConsumerPayloadShape.Array, "gameId", "name"),
        new("getPlayerLocation", ConsumerPayloadShape.Object, "x", "y", "z"),
        new("getPlayerInventory", ConsumerPayloadShape.Array, "code", "name"),
        new("giveItem", ConsumerPayloadShape.Unvalidated),
        new("sendMessage", ConsumerPayloadShape.Unvalidated),
        new("executeConsoleCommand", ConsumerPayloadShape.Object, "rawResult", "success"),
        new("listItems", ConsumerPayloadShape.Array, "code", "name"),
        new("listEntities", ConsumerPayloadShape.Array, "code", "name"),
        new("listLocations", ConsumerPayloadShape.Array, "position", "name"),
        new("getMapInfo", ConsumerPayloadShape.Object, "enabled", "mapBlockSize", "maxZoom", "mapSizeX", "mapSizeY", "mapSizeZ"),
        new("getMapTile", ConsumerPayloadShape.Unvalidated),
        new("teleportPlayer", ConsumerPayloadShape.Unvalidated),
        new("kickPlayer", ConsumerPayloadShape.Unvalidated),
        new("banPlayer", ConsumerPayloadShape.Unvalidated),
        new("unbanPlayer", ConsumerPayloadShape.Unvalidated),
        new("listBans", ConsumerPayloadShape.Array, "player", "reason"),
        new("testReachability", ConsumerPayloadShape.Object, "connectable"),
        new("shutdown", ConsumerPayloadShape.Unvalidated)
    };

    // Pinned from @takaro/modules 0.4.17 EventEntityKilled plus BaseEvent.
    // This local mirror intentionally avoids a runtime dependency on node_modules.
    private static readonly ConsumerEventContract[] PinnedEventValidationMap =
    {
        new(
            "entity-killed",
            new ConsumerEventProperty("player", JsonValueKind.Object),
            new ConsumerEventProperty("entity", JsonValueKind.String),
            new ConsumerEventProperty("weapon", JsonValueKind.String),
            new ConsumerEventProperty("timestamp", JsonValueKind.String))
    };

    [TestMethod]
    public void PinnedTakaroValidationMapCoversEverySupportedActionAt0c63cf1c()
    {
        var expectedActions = new[]
        {
            "testReachability", "getPlayers", "getPlayer", "getPlayerLocation",
            "getPlayerInventory", "giveItem", "sendMessage", "executeConsoleCommand",
            "listItems", "listEntities", "listLocations", "teleportPlayer",
            "getMapInfo", "getMapTile", "kickPlayer", "banPlayer", "unbanPlayer", "listBans", "shutdown"
        };

        CollectionAssert.AreEquivalent(
            expectedActions,
            PinnedValidationMap.Select(contract => contract.Action).ToArray());
    }

    [TestMethod]
    public void PinnedTakaroValidationMapAcceptsKnownGoodPayloadsAt0c63cf1c()
    {
        var samples = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["getPlayer"] = """{"gameId":"Steam_1","name":"Odin"}""",
            ["getPlayers"] = """[{"gameId":"Steam_1","name":"Odin"}]""",
            ["getPlayerLocation"] = """{"x":1,"y":2,"z":3}""",
            ["getPlayerInventory"] = """[{"code":"Wood","name":"Wood"}]""",
            ["giveItem"] = """{}""",
            ["sendMessage"] = """{}""",
            ["executeConsoleCommand"] = """{"rawResult":"ok","success":true}""",
            ["listItems"] = """[{"code":"Wood","name":"Wood"}]""",
            ["listEntities"] = """[{"code":"Boar","name":"Boar"}]""",
            ["listLocations"] = """[{"name":"StartTemple","position":{"x":0,"y":0,"z":0}}]""",
            ["getMapInfo"] = """{"enabled":false,"mapBlockSize":0,"maxZoom":0,"mapSizeX":0,"mapSizeY":0,"mapSizeZ":0}""",
            ["getMapTile"] = """{}""",
            ["teleportPlayer"] = """{}""",
            ["kickPlayer"] = """null""",
            ["banPlayer"] = """null""",
            ["unbanPlayer"] = """null""",
            ["listBans"] = """[{"player":{"gameId":"Steam_1","name":"Odin"},"reason":""}]""",
            ["testReachability"] = """{"connectable":true}""",
            ["shutdown"] = """null"""
        };

        foreach (var contract in PinnedValidationMap)
        {
            using var document = JsonDocument.Parse(samples[contract.Action]);
            AssertPinnedTakaroValidationAccepts(contract.Action, document.RootElement);
        }
    }

    [DataTestMethod]
    [DataRow("getPlayer")]
    [DataRow("getMapInfo")]
    [DataRow("getMapTile")]
    [DataRow("giveItem")]
    [DataRow("sendMessage")]
    [DataRow("executeConsoleCommand")]
    [DataRow("teleportPlayer")]
    [DataRow("kickPlayer")]
    [DataRow("banPlayer")]
    [DataRow("unbanPlayer")]
    [DataRow("shutdown")]
    public void FailureCapableActionsReturnOneSchemaValidPayloadErrorFrame(string action)
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            $"failure-{action}",
            action,
            TakaroActionResult.Error("invalid_args", "Amount must be an integer."),
            out var frame);

        Assert.IsTrue(shouldSend, action);
        Assert.IsNotNull(frame, action);

        using var document = JsonDocument.Parse(frame!);
        var payload = document.RootElement.GetProperty("payload");
        AssertPinnedTakaroValidationAccepts(action, payload);

        var exception = Assert.ThrowsException<InvalidOperationException>(
            () => ResolveLikeCurrentTakaro(frame!),
            action);
        StringAssert.Contains(exception.Message, "invalid_args", action);
        StringAssert.Contains(exception.Message, "Amount must be an integer", action);
    }

    [TestMethod]
    public void UnsupportedMapInfoUsesEveryRequiredFieldAndAnActionablePayloadError()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "map-info",
            "getMapInfo",
            TakaroActionResult.Error("server_only_unsupported", "Valheim dedicated servers do not expose map tiles."),
            out var frame);

        Assert.IsTrue(shouldSend);
        using var document = JsonDocument.Parse(frame!);
        var payload = document.RootElement.GetProperty("payload");
        AssertPinnedTakaroValidationAccepts("getMapInfo", payload);
        Assert.IsFalse(payload.GetProperty("enabled").GetBoolean());
        StringAssert.Contains(payload.GetProperty("error").GetString(), "server_only_unsupported");
    }

    [DataTestMethod]
    [DataRow("getPlayers")]
    [DataRow("getPlayerInventory")]
    [DataRow("listItems")]
    [DataRow("listEntities")]
    [DataRow("listLocations")]
    [DataRow("listBans")]
    public void ArrayValidatedFailuresDoNotFabricateAConsumerPayload(string action)
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            $"failure-{action}",
            action,
            TakaroActionResult.Error("action_failed", "Server-owned data unavailable."),
            out var frame);

        Assert.IsFalse(shouldSend, action);
        Assert.IsNull(frame, action);
    }

    [TestMethod]
    public void ReachabilityFailureReturnsImmediateSchemaValidDisconnectedReason()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "reachability-failure",
            "testReachability",
            TakaroActionResult.Error("socket_unavailable", "The Valheim socket is unavailable."),
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        using var document = JsonDocument.Parse(frame!);
        var payload = document.RootElement.GetProperty("payload");
        AssertPinnedTakaroValidationAccepts("testReachability", payload);
        Assert.IsFalse(payload.GetProperty("connectable").GetBoolean());
        StringAssert.Contains(payload.GetProperty("reason").GetString(), "socket_unavailable");
        StringAssert.Contains(payload.GetProperty("error").GetString(), "socket_unavailable");
    }

    [TestMethod]
    public void TakaroResolvesOnlyPayloadAndIgnoresRootFailureMetadata()
    {
        const string oldInventoryFallback = """
        {
          "type": "response",
          "requestId": "inventory",
          "payload": [],
          "success": false,
          "errorCode": "player_component_unavailable",
          "message": "Remote inventory is unavailable."
        }
        """;

        var resolved = ResolveLikeCurrentTakaro(oldInventoryFallback);

        Assert.AreEqual(JsonValueKind.Array, resolved.ValueKind);
        Assert.AreEqual(0, resolved.GetArrayLength());
    }

    [TestMethod]
    public void UnsupportedInventoryProducesNoFrameForTakaroToResolve()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "inventory",
            "getPlayerInventory",
            TakaroActionResult.Error("player_component_unavailable", "Dedicated servers do not expose remote inventory."),
            out var frame);

        Assert.IsFalse(shouldSend);
        Assert.IsNull(frame);
    }

    [TestMethod]
    public void CompanionProcessorOutputsMatchPinnedTakaroGameEventShapes()
    {
        const long peerId = 42;
        const string nonce = "server-session";
        var now = DateTimeOffset.Parse("2026-07-11T12:00:00+00:00");
        var player = new TakaroPlayer("Steam_real", "Odin", "real", "steam:real", null, null);
        var capabilities = CompanionCapability.Chat
            | CompanionCapability.PlayerDeath
            | CompanionCapability.EntityKilled;
        var sessions = new CompanionSessionRegistry(
            CompanionProtocol.CurrentVersion,
            CompanionProtocol.CurrentVersion,
            capabilities,
            TimeSpan.FromSeconds(5),
            TimeSpan.FromMinutes(1));
        sessions.Begin(peerId, now, nonce);
        Assert.AreEqual(
            CompanionSessionDecision.Accept,
            sessions.CompleteHelloAck(
                peerId,
                nonce,
                CompanionProtocol.CurrentVersion,
                "1.0.0",
                capabilities,
                sequence: 1,
                now));
        var processor = new CompanionReportProcessor(
            sessions,
            new CompanionRateLimiter(10, 1, TimeSpan.FromMinutes(1)),
            new BoundedEventDeduplicator(10),
            new CompanionInventoryCache());

        var outputs = new[]
        {
            processor.Process(
                peerId,
                player,
                ReportEnvelope(
                    nonce,
                    sequence: 2,
                    CompanionMessageTypes.Chat,
                    new CompanionChatReport("chat-1", now.ToUnixTimeMilliseconds(), "hello")),
                now),
            processor.Process(
                peerId,
                player,
                ReportEnvelope(
                    nonce,
                    sequence: 3,
                    CompanionMessageTypes.PlayerDeath,
                    new CompanionPlayerDeathReport(
                        "death-1",
                        now.ToUnixTimeMilliseconds(),
                        new CompanionPosition(1, 2, 3),
                        null,
                        null)),
                now),
            processor.Process(
                peerId,
                player,
                ReportEnvelope(
                    nonce,
                    sequence: 4,
                    CompanionMessageTypes.EntityKilled,
                    new CompanionEntityKilledReport(
                        "kill-1",
                        now.ToUnixTimeMilliseconds(),
                        new CompanionPosition(4, 5, 6),
                        "Greydwarf",
                        "SwordIron")),
                now)
        };

        var events = outputs.Select(RequireAcceptedEvent).ToArray();
        CollectionAssert.AreEqual(
            new[] { "chat-message", "player-death", "entity-killed" },
            events.Select(gameEvent => gameEvent.Type).ToArray());

        using var chat = ParseGameEvent(events[0]);
        AssertPropertySet(
            chat.RootElement.GetProperty("payload").GetProperty("data"),
            "channel", "msg", "player", "timestamp");
        Assert.AreEqual(
            "Steam_real",
            chat.RootElement.GetProperty("payload").GetProperty("data").GetProperty("player").GetProperty("gameId").GetString());

        using var death = ParseGameEvent(events[1]);
        AssertPropertySet(
            death.RootElement.GetProperty("payload").GetProperty("data"),
            "player", "position", "timestamp");
        Assert.IsFalse(
            death.RootElement.GetProperty("payload").GetProperty("data").TryGetProperty("attacker", out _));

        using var killed = ParseGameEvent(events[2]);
        var killedData = killed.RootElement.GetProperty("payload").GetProperty("data");
        AssertPropertySet(killedData, "entity", "player", "timestamp", "weapon");
        Assert.IsTrue(PinnedTakaroEventValidationAccepts("entity-killed", killedData));
        Assert.AreEqual("Steam_real", killedData.GetProperty("player").GetProperty("gameId").GetString());
        Assert.AreEqual("Greydwarf", killedData.GetProperty("entity").GetString());
        Assert.AreEqual("SwordIron", killedData.GetProperty("weapon").GetString());
        Assert.AreEqual("2026-07-11T12:00:00+00:00", killedData.GetProperty("timestamp").GetString());
    }

    [TestMethod]
    public void PinnedEntityKilledContractRejectsMissingWeaponAndPositionExtension()
    {
        using var valid = JsonDocument.Parse(
            """{"player":{"gameId":"Steam_1","name":"Odin"},"entity":"Greydwarf","weapon":"SwordIron","timestamp":"2026-07-11T12:00:00+00:00"}""");
        using var missingWeapon = JsonDocument.Parse(
            """{"player":{"gameId":"Steam_1","name":"Odin"},"entity":"Greydwarf","timestamp":"2026-07-11T12:00:00+00:00"}""");
        using var extraPosition = JsonDocument.Parse(
            """{"player":{"gameId":"Steam_1","name":"Odin"},"entity":"Greydwarf","weapon":"SwordIron","timestamp":"2026-07-11T12:00:00+00:00","position":{"x":1,"y":2,"z":3}}""");

        Assert.IsTrue(PinnedTakaroEventValidationAccepts("entity-killed", valid.RootElement));
        Assert.IsFalse(PinnedTakaroEventValidationAccepts("entity-killed", missingWeapon.RootElement));
        Assert.IsFalse(PinnedTakaroEventValidationAccepts("entity-killed", extraPosition.RootElement));
    }

    [DataTestMethod]
    [DataRow("1001")]
    [DataRow("1.5")]
    public async Task InvalidGiveItemAmountReturnsOneImmediateActionableFrame(string amountJson)
    {
        var dispatcher = new TakaroRequestDispatcher(new NeverCalledAdapter());
        using var args = JsonDocument.Parse($$"""{"gameId":"Steam_1","item":"Wood","amount":{{amountJson}}}""");
        var result = await dispatcher.DispatchAsync(new TakaroRequest("invalid-give", "giveItem", args.RootElement));

        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "invalid-give",
            "giveItem",
            result,
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        using var document = JsonDocument.Parse(frame!);
        Assert.AreEqual("response", document.RootElement.GetProperty("type").GetString());
        var error = document.RootElement.GetProperty("payload").GetProperty("error").GetString();
        StringAssert.Contains(error, "invalid_args");
    }

    [DataTestMethod]
    [DataRow("player_position_unavailable")]
    [DataRow("player_not_found")]
    public void UnavailableLocationUsesSchemaValidPayloadErrorThatTakaroRejects(string errorCode)
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "location",
            "getPlayerLocation",
            TakaroActionResult.Error(errorCode, "No real server-observed position is available."),
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        var exception = Assert.ThrowsException<InvalidOperationException>(() => ResolveLikeCurrentTakaro(frame!));
        StringAssert.Contains(exception.Message, errorCode);

        using var document = JsonDocument.Parse(frame!);
        var payload = document.RootElement.GetProperty("payload");
        Assert.AreEqual(0, payload.GetProperty("x").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("y").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("z").GetDouble());
        Assert.IsTrue(payload.TryGetProperty("error", out _));
        Assert.IsFalse(document.RootElement.TryGetProperty("success", out _));
        Assert.IsFalse(document.RootElement.TryGetProperty("errorCode", out _));
    }

    [TestMethod]
    public void RealLocationProducesExactlyOneResolvablePayload()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "location",
            "getPlayerLocation",
            TakaroActionResult.Ok(new TakaroPosition(12, 34, 56, "valheim")),
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        var resolved = ResolveLikeCurrentTakaro(frame!);
        Assert.AreEqual(12, resolved.GetProperty("x").GetDouble());
        Assert.AreEqual(34, resolved.GetProperty("y").GetDouble());
        Assert.AreEqual(56, resolved.GetProperty("z").GetDouble());
        Assert.IsFalse(resolved.TryGetProperty("error", out _));
    }

    [TestMethod]
    public void SuppressedResponseLogsAreRateLimitedPerFailure()
    {
        var limiter = new SuppressedResponseLogLimiter(TimeSpan.FromSeconds(30));
        var now = DateTimeOffset.Parse("2026-07-10T15:30:00+02:00");

        Assert.IsTrue(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now));
        Assert.IsFalse(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now.AddSeconds(29)));
        Assert.IsTrue(limiter.ShouldLog("giveItem", "invalid_amount", now.AddSeconds(1)));
        Assert.IsTrue(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now.AddSeconds(30)));
    }

    private static JsonElement ResolveLikeCurrentTakaro(string responseFrame)
    {
        using var document = JsonDocument.Parse(responseFrame);
        var payload = document.RootElement.GetProperty("payload");
        if (payload.ValueKind == JsonValueKind.Object
            && payload.TryGetProperty("error", out var error)
            && error.ValueKind == JsonValueKind.String)
        {
            throw new InvalidOperationException(error.GetString());
        }

        return payload.Clone();
    }

    private static CompanionEnvelope ReportEnvelope<T>(
        string nonce,
        long sequence,
        string type,
        T payload) =>
        new(
            CompanionProtocol.CurrentVersion,
            nonce,
            sequence,
            $"message-{sequence}",
            type,
            JsonSerializer.SerializeToElement(
                payload,
                new JsonSerializerOptions(JsonSerializerDefaults.Web)));

    private static JsonDocument ParseGameEvent(CompanionAcceptedEvent gameEvent) =>
        JsonDocument.Parse(TakaroProtocol.CreateGameEvent(gameEvent.Type, gameEvent.Data));

    private static CompanionAcceptedEvent RequireAcceptedEvent(CompanionReportOutput? output)
    {
        Assert.IsInstanceOfType<CompanionAcceptedEvent>(output);
        return (CompanionAcceptedEvent)output!;
    }

    private static void AssertPropertySet(JsonElement element, params string[] expected)
    {
        CollectionAssert.AreEquivalent(
            expected,
            element.EnumerateObject().Select(property => property.Name).ToArray());
    }

    private static void AssertPinnedTakaroValidationAccepts(string action, JsonElement payload)
    {
        var contract = PinnedValidationMap.Single(candidate => candidate.Action == action);
        if (contract.Shape == ConsumerPayloadShape.Unvalidated)
        {
            return;
        }

        Assert.AreEqual(
            contract.Shape == ConsumerPayloadShape.Array ? JsonValueKind.Array : JsonValueKind.Object,
            payload.ValueKind,
            $"Pinned Takaro 0c63cf1c top-level payload shape for {action}");

        var dto = contract.Shape == ConsumerPayloadShape.Array
            ? payload.EnumerateArray().FirstOrDefault()
            : payload;
        foreach (var property in contract.RequiredProperties)
        {
            Assert.IsTrue(dto.TryGetProperty(property, out var value), $"{action} is missing required DTO field {property}");
            var expectedKind = property switch
            {
                "x" or "y" or "z" => JsonValueKind.Number,
                "connectable" or "success" or "enabled" => JsonValueKind.True,
                "mapBlockSize" or "maxZoom" or "mapSizeX" or "mapSizeY" or "mapSizeZ" => JsonValueKind.Number,
                "position" or "player" => JsonValueKind.Object,
                _ => JsonValueKind.String
            };
            if (expectedKind == JsonValueKind.True)
            {
                Assert.IsTrue(
                    value.ValueKind is JsonValueKind.True or JsonValueKind.False,
                    $"{action}.{property} must be boolean at Takaro 0c63cf1c");
            }
            else
            {
                Assert.AreEqual(expectedKind, value.ValueKind, $"{action}.{property} at Takaro 0c63cf1c");
            }
        }


        if (action == "listLocations")
        {
            var position = dto.GetProperty("position");
            foreach (var coordinate in new[] { "x", "y", "z" })
            {
                Assert.AreEqual(JsonValueKind.Number, position.GetProperty(coordinate).ValueKind, $"{action}.{coordinate}");
            }
        }

        if (action == "listBans")
        {
            var player = dto.GetProperty("player");
            Assert.AreEqual(JsonValueKind.String, player.GetProperty("gameId").ValueKind);
            Assert.AreEqual(JsonValueKind.String, player.GetProperty("name").ValueKind);
        }
    }

    private static bool PinnedTakaroEventValidationAccepts(string eventType, JsonElement data)
    {
        var contract = PinnedEventValidationMap.SingleOrDefault(candidate => candidate.EventType == eventType);
        if (contract is null || data.ValueKind != JsonValueKind.Object)
        {
            return false;
        }

        var properties = data.EnumerateObject().ToArray();
        if (properties.Length != contract.RequiredProperties.Length)
        {
            return false;
        }

        foreach (var required in contract.RequiredProperties)
        {
            if (!data.TryGetProperty(required.Name, out var value)
                || value.ValueKind != required.Kind)
            {
                return false;
            }
        }

        var player = data.GetProperty("player");
        return player.TryGetProperty("gameId", out var gameId)
            && gameId.ValueKind == JsonValueKind.String
            && player.TryGetProperty("name", out var name)
            && name.ValueKind == JsonValueKind.String
            && !string.IsNullOrWhiteSpace(data.GetProperty("entity").GetString())
            && !string.IsNullOrWhiteSpace(data.GetProperty("weapon").GetString())
            && data.GetProperty("timestamp").TryGetDateTimeOffset(out _);
    }

    private enum ConsumerPayloadShape
    {
        Unvalidated,
        Object,
        Array
    }

    private sealed record ConsumerActionContract(
        string Action,
        ConsumerPayloadShape Shape,
        params string[] RequiredProperties);

    private sealed record ConsumerEventProperty(string Name, JsonValueKind Kind);

    private sealed record ConsumerEventContract(
        string EventType,
        params ConsumerEventProperty[] RequiredProperties);

    private sealed class NeverCalledAdapter : IValheimTakaroAdapter
    {
        private static Exception Unexpected() => new InvalidOperationException("Dispatcher must reject before adapter invocation.");

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, string? senderNameOverride, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => throw Unexpected();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => throw Unexpected();
    }
}
