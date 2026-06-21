using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class RequestDispatcherTests
{
    [TestMethod]
    public async Task DispatchesAdminMvpActionsThroughAdapter()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);

        await dispatcher.DispatchAsync(new TakaroRequest("1", "sendMessage", JsonDocument.Parse("""{"message":"hello"}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("2", "executeConsoleCommand", JsonDocument.Parse("""{"command":"players"}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("3", "kickPlayer", JsonDocument.Parse("""{"gameId":"Steam_1","reason":"spam"}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("4", "shutdown", JsonDocument.Parse("""{}""").RootElement));

        CollectionAssert.AreEqual(
            new[] { "message:hello:<global>", "command:players", "kick:Steam_1:spam", "shutdown" },
            adapter.Calls);
    }

    [TestMethod]
    public async Task DispatchesOfficialNestedPlayerArgsThroughAdapter()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);

        await dispatcher.DispatchAsync(new TakaroRequest("8", "getPlayer", JsonDocument.Parse("""{"player":{"gameId":"Steam_1"}}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("9", "getPlayerLocation", JsonDocument.Parse("""{"player":{"platformId":"steam:76561198000000001"}}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("10", "getPlayerInventory", JsonDocument.Parse("""{"player":{"steamId":"76561198000000001"}}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("11", "kickPlayer", JsonDocument.Parse("""{"player":{"gameId":"Steam_2"},"reason":"spam"}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("12", "banPlayer", JsonDocument.Parse("""{"player":{"gameId":"Steam_3"},"reason":"cheating","expiresAt":"2031-12-31T23:59:59Z"}""").RootElement));
        await dispatcher.DispatchAsync(new TakaroRequest("13", "unbanPlayer", JsonDocument.Parse("""{"gameId":"Steam_3"}""").RootElement));

        CollectionAssert.AreEqual(
            new[]
            {
                "getPlayer:Steam_1",
                "location:steam:76561198000000001",
                "inventory:76561198000000001",
                "kick:Steam_2:spam",
                "ban:Steam_3:cheating",
                "unban:Steam_3"
            },
            adapter.Calls);
    }

    [TestMethod]
    public async Task DispatchesSendMessageRecipientWithoutBroadcasting()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);

        await dispatcher.DispatchAsync(new TakaroRequest(
            "14",
            "sendMessage",
            JsonDocument.Parse("""{"message":"Welcome","opts":{"recipient":{"gameId":"Steam_1"}}}""").RootElement));

        CollectionAssert.AreEqual(
            new[] { "message:Welcome:Steam_1" },
            adapter.Calls);
    }

    [TestMethod]
    public async Task DispatchesGiveItemThroughAdapter()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);

        var response = await dispatcher.DispatchAsync(new TakaroRequest(
            "give",
            "giveItem",
            JsonDocument.Parse("""{"player":{"gameId":"Steam_1"},"item":"Wood","amount":10}""").RootElement));

        Assert.IsTrue(response.Success);
        CollectionAssert.AreEqual(new[] { "give:Steam_1:Wood:10:<quality>" }, adapter.Calls);
    }

    [TestMethod]
    public async Task DispatchesTeleportPlayerThroughAdapter()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);

        var response = await dispatcher.DispatchAsync(new TakaroRequest(
            "teleport",
            "teleportPlayer",
            JsonDocument.Parse("""{"player":{"gameId":"Steam_1"},"x":100,"y":42,"z":-20}""").RootElement));

        Assert.IsTrue(response.Success);
        CollectionAssert.AreEqual(new[] { "teleport:Steam_1:100:42:-20" }, adapter.Calls);
    }

    [TestMethod]
    public async Task ListActionsReturnBareArrays()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());

        var itemsResult = await dispatcher.DispatchAsync(new TakaroRequest("15", "listItems", JsonDocument.Parse("""[]""").RootElement));
        var itemsResponse = TakaroProtocol.CreateResponse("15", itemsResult);
        using var itemsDocument = JsonDocument.Parse(itemsResponse);
        var itemsPayload = itemsDocument.RootElement.GetProperty("payload");
        Assert.AreEqual(JsonValueKind.Array, itemsPayload.ValueKind);
        Assert.AreEqual("Wood", itemsPayload[0].GetProperty("code").GetString());
        Assert.AreEqual("Wood", itemsPayload[0].GetProperty("name").GetString());
        Assert.AreEqual(1, itemsPayload[0].GetProperty("amount").GetInt32());
        Assert.AreEqual("1", itemsPayload[0].GetProperty("quality").GetString());

        var entitiesResult = await dispatcher.DispatchAsync(new TakaroRequest("16", "listEntities", JsonDocument.Parse("""[]""").RootElement));
        var entitiesResponse = TakaroProtocol.CreateResponse("16", entitiesResult);
        using var entitiesDocument = JsonDocument.Parse(entitiesResponse);
        var entitiesPayload = entitiesDocument.RootElement.GetProperty("payload");
        Assert.AreEqual(JsonValueKind.Array, entitiesPayload.ValueKind);
        Assert.AreEqual("Boar", entitiesPayload[0].GetProperty("code").GetString());
        Assert.AreEqual("Boar", entitiesPayload[0].GetProperty("name").GetString());

        var locationsResult = await dispatcher.DispatchAsync(new TakaroRequest("17", "listLocations", JsonDocument.Parse("""[]""").RootElement));
        var locationsResponse = TakaroProtocol.CreateResponse("17", locationsResult);
        using var locationsDocument = JsonDocument.Parse(locationsResponse);
        Assert.AreEqual(JsonValueKind.Array, locationsDocument.RootElement.GetProperty("payload").ValueKind);

        var bansResult = await dispatcher.DispatchAsync(new TakaroRequest("18", "listBans", JsonDocument.Parse("""[]""").RootElement));
        var bansResponse = TakaroProtocol.CreateResponse("18", bansResult);
        using var bansDocument = JsonDocument.Parse(bansResponse);
        Assert.AreEqual(JsonValueKind.Array, bansDocument.RootElement.GetProperty("payload").ValueKind);
    }

    [TestMethod]
    public async Task AllTakaroActionsReturnExpectedBarePayloadShapes()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());
        var cases = new[]
        {
            new ActionCase("testReachability", """{}""", JsonValueKind.Object, "connectable"),
            new ActionCase("getPlayers", """{}""", JsonValueKind.Array, "gameId"),
            new ActionCase("getPlayer", """{"gameId":"Steam_1"}""", JsonValueKind.Object, "gameId"),
            new ActionCase("getPlayerLocation", """{"gameId":"Steam_1"}""", JsonValueKind.Object, "x"),
            new ActionCase("getPlayerInventory", """{"gameId":"Steam_1"}""", JsonValueKind.Array, "code"),
            new ActionCase("giveItem", """{"gameId":"Steam_1","item":"Wood","amount":1}""", JsonValueKind.Object, "queued"),
            new ActionCase("listItems", """{}""", JsonValueKind.Array, "code"),
            new ActionCase("listEntities", """{}""", JsonValueKind.Array, "code"),
            new ActionCase("listLocations", """{}""", JsonValueKind.Array, "code"),
            new ActionCase("executeConsoleCommand", """{"command":"help"}""", JsonValueKind.Object, "rawResult", AllowsSuccessProperty: true),
            new ActionCase("sendMessage", """{"message":"hello"}""", JsonValueKind.Object, "sent"),
            new ActionCase("teleportPlayer", """{"gameId":"Steam_1","position":{"x":1,"y":2,"z":3}}""", JsonValueKind.Object, "queued"),
            new ActionCase("kickPlayer", """{"gameId":"Steam_1"}""", JsonValueKind.Null, null),
            new ActionCase("banPlayer", """{"gameId":"Steam_1"}""", JsonValueKind.Null, null),
            new ActionCase("unbanPlayer", """{"gameId":"Steam_1"}""", JsonValueKind.Null, null),
            new ActionCase("listBans", """{}""", JsonValueKind.Array, "player"),
            new ActionCase("shutdown", """{}""", JsonValueKind.Null, null)
        };

        foreach (var testCase in cases)
        {
            var result = await dispatcher.DispatchAsync(new TakaroRequest(
                testCase.Action,
                testCase.Action,
                JsonDocument.Parse(testCase.ArgsJson).RootElement));
            using var document = JsonDocument.Parse(TakaroProtocol.CreateResponse(testCase.Action, result));
            var payload = document.RootElement.GetProperty("payload");

            Assert.AreEqual(testCase.ExpectedKind, payload.ValueKind, testCase.Action);
            Assert.IsFalse(
                payload.ValueKind == JsonValueKind.Object
                && !testCase.AllowsSuccessProperty
                && payload.TryGetProperty("success", out _),
                testCase.Action);
            if (testCase.RequiredProperty is not null)
            {
                var target = payload.ValueKind == JsonValueKind.Array ? payload[0] : payload;
                Assert.IsTrue(target.TryGetProperty(testCase.RequiredProperty, out _), $"{testCase.Action} missing {testCase.RequiredProperty}");
            }
        }
    }

    [TestMethod]
    public async Task UnsupportedActionsReturnStructuredError()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());

        var response = await dispatcher.DispatchAsync(new TakaroRequest("4", "notImplementedYet", JsonDocument.Parse("""{}""").RootElement));

        Assert.IsFalse(response.Success);
        Assert.AreEqual("unsupported_action", response.ErrorCode);
        StringAssert.Contains(response.Message, "notImplementedYet");
    }

    [TestMethod]
    public async Task TestReachabilityResponseUsesTakaroConnectableShape()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());

        var actionResult = await dispatcher.DispatchAsync(new TakaroRequest("5", "testReachability", JsonDocument.Parse("""{}""").RootElement));
        var response = TakaroProtocol.CreateResponse("5", actionResult);
        using var document = JsonDocument.Parse(response);
        var payload = document.RootElement.GetProperty("payload");

        Assert.IsTrue(payload.GetProperty("connectable").GetBoolean());
        Assert.IsFalse(payload.TryGetProperty("reachable", out _));
        Assert.IsFalse(payload.TryGetProperty("success", out _));
    }

    [TestMethod]
    public async Task PlayerDetailActionsUseTakaroExpectedPayloadShapes()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());

        var inventoryResult = await dispatcher.DispatchAsync(new TakaroRequest("6", "getPlayerInventory", JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));
        var inventoryResponse = TakaroProtocol.CreateResponse("6", inventoryResult);
        using var inventoryDocument = JsonDocument.Parse(inventoryResponse);
        var inventoryPayload = inventoryDocument.RootElement.GetProperty("payload");
        Assert.AreEqual(JsonValueKind.Array, inventoryPayload.ValueKind);
        Assert.AreEqual("Wood", inventoryPayload[0].GetProperty("code").GetString());
        Assert.AreEqual("Wood", inventoryPayload[0].GetProperty("name").GetString());
        Assert.AreEqual("1", inventoryPayload[0].GetProperty("quality").GetString());

        var locationResult = await dispatcher.DispatchAsync(new TakaroRequest("7", "getPlayerLocation", JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));
        var locationResponse = TakaroProtocol.CreateResponse("7", locationResult);
        using var locationDocument = JsonDocument.Parse(locationResponse);
        var locationPayload = locationDocument.RootElement.GetProperty("payload");
        Assert.AreEqual(1, locationPayload.GetProperty("x").GetInt32());
        Assert.AreEqual(2, locationPayload.GetProperty("y").GetInt32());
        Assert.AreEqual(3, locationPayload.GetProperty("z").GetInt32());
    }

    [TestMethod]
    public async Task ModerationSuccessActionsUseTakaroNullPayloadShape()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());

        var kickResult = await dispatcher.DispatchAsync(new TakaroRequest("kick", "kickPlayer", JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));
        using var kickDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("kick", kickResult));
        Assert.AreEqual(JsonValueKind.Null, kickDocument.RootElement.GetProperty("payload").ValueKind);

        var banResult = await dispatcher.DispatchAsync(new TakaroRequest("ban", "banPlayer", JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));
        using var banDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("ban", banResult));
        Assert.AreEqual(JsonValueKind.Null, banDocument.RootElement.GetProperty("payload").ValueKind);

        var unbanResult = await dispatcher.DispatchAsync(new TakaroRequest("unban", "unbanPlayer", JsonDocument.Parse("""{"gameId":"Steam_1"}""").RootElement));
        using var unbanDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("unban", unbanResult));
        Assert.AreEqual(JsonValueKind.Null, unbanDocument.RootElement.GetProperty("payload").ValueKind);

        var shutdownResult = await dispatcher.DispatchAsync(new TakaroRequest("shutdown", "shutdown", JsonDocument.Parse("""{}""").RootElement));
        using var shutdownDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("shutdown", shutdownResult));
        Assert.AreEqual(JsonValueKind.Null, shutdownDocument.RootElement.GetProperty("payload").ValueKind);
    }

    private sealed class FakeAdapter : IValheimTakaroAdapter
    {
        public List<string> Calls { get; } = [];

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new { connectable = true }));

        public Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<TakaroPlayer>>(new[]
            {
                new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null)
            });

        public Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
            RecordCall($"getPlayer:{identifier}", Task.FromResult<TakaroPlayer?>(new TakaroPlayer("Steam_1", "Odin", "1", "steam:1", null, null)));

        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) =>
            RecordCall($"location:{identifier}", Task.FromResult(TakaroActionResult.Ok(new { x = 1, y = 2, z = 3 })));

        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) =>
            RecordCall($"inventory:{identifier}", Task.FromResult(TakaroActionResult.Ok(new[] { new { code = "Wood", name = "Wood", amount = 50, quality = "1" } })));

        public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) =>
            RecordCall($"give:{identifier}:{itemCode}:{amount}:{quality ?? "<quality>"}", Task.FromResult(TakaroActionResult.Ok(new { queued = true })));

        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, CancellationToken cancellationToken = default)
        {
            Calls.Add($"message:{message}:{recipientIdentifier ?? "<global>"}");
            return Task.FromResult(TakaroActionResult.Ok(new { sent = true }));
        }

        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default)
        {
            Calls.Add($"command:{command}");
            return Task.FromResult(TakaroActionResult.Ok(new { success = true, rawResult = $"Executed: {command}" }));
        }

        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default)
        {
            Calls.Add($"kick:{identifier}:{reason}");
            return Task.FromResult(TakaroActionResult.Ok(new { kicked = true }));
        }

        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) =>
            RecordCall($"ban:{identifier}:{reason}", Task.FromResult(TakaroActionResult.Ok(new { banned = true })));

        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
            RecordCall($"unban:{identifier}", Task.FromResult(TakaroActionResult.Ok(new { unbanned = true })));

        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(ModerationFactory.CreateBanEntries(new[]
            {
                new ValheimBan("Steam_1", "Odin", "1", "steam:1")
            })));

        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new[] { new { code = "Wood", name = "Wood", amount = 1, quality = "1" } }));

        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new[] { new { code = "Boar", name = "Boar" } }));

        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new[] { LocationFactory.Create("StartTemple", "Start Temple", 0, 0, 0) }));

        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) =>
            RecordCall($"teleport:{identifier}:{position.X}:{position.Y}:{position.Z}", Task.FromResult(TakaroActionResult.Ok(new { queued = true })));

        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) =>
            RecordCall("shutdown", Task.FromResult(TakaroActionResult.Ok(new { queued = true })));

        private T RecordCall<T>(string call, T result)
        {
            Calls.Add(call);
            return result;
        }
    }

    private sealed record ActionCase(
        string Action,
        string ArgsJson,
        JsonValueKind ExpectedKind,
        string? RequiredProperty,
        bool AllowsSuccessProperty = false);
}
