using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class RequestDispatcherTests
{
    [TestMethod]
    public async Task DispatchesSupportedActionsToAdapter()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);
        var request = Request("sendMessage", """{"message":"hello"}""");

        var result = await dispatcher.DispatchAsync(request);

        Assert.IsTrue(result.Success);
        Assert.AreEqual("hello", adapter.LastMessage);
    }

    [TestMethod]
    public async Task RecipientAndSenderArePassedWithoutBroadcasting()
    {
        var adapter = new FakeAdapter();
        var result = await new TakaroRequestDispatcher(adapter).DispatchAsync(Request("sendMessage",
            """{"message":"private","opts":{"recipient":{"gameId":"76561198000000001"},"senderNameOverride":"Takaro"}}"""));
        Assert.IsTrue(result.Success);
        Assert.AreEqual("76561198000000001", adapter.LastRecipient);
        Assert.AreEqual("Takaro", adapter.LastSender);
    }

    [TestMethod]
    public async Task NestedAndFlatPlayerIdentifiersResolveForNewActions()
    {
        var adapter = new FakeAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter);
        var nested = await dispatcher.DispatchAsync(Request("getPlayerLocation", """{"player":{"gameId":"76561198000000001"}}"""));
        Assert.IsFalse(nested.Success);
        Assert.AreEqual("76561198000000001", adapter.LastIdentifier);
        var flat = await dispatcher.DispatchAsync(Request("getPlayerInventory", """{"gameId":"76561198000000001"}"""));
        Assert.IsFalse(flat.Success);
        Assert.AreEqual("76561198000000001", adapter.LastIdentifier);
    }

    [TestMethod]
    public async Task InvalidPositionFailsBeforeGameMutation()
    {
        var adapter = new FakeAdapter();
        var result = await new TakaroRequestDispatcher(adapter).DispatchAsync(Request("teleportPlayer",
            """{"gameId":"76561198000000001","position":{"x":1,"y":2}}"""));
        Assert.AreEqual("invalid_args", result.Error?.Code);
        Assert.IsNull(adapter.LastIdentifier);
    }

    [TestMethod]
    public async Task ReturnsInvalidArgsWhenRequiredCommandIsMissing()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());
        var request = Request("executeConsoleCommand", "{}");

        var result = await dispatcher.DispatchAsync(request);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("invalid_args", result.Error?.Code);
    }

    [TestMethod]
    public async Task ReturnsUnsupportedActionForOutOfScopeFunctions()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());
        var request = Request("listEntities", """{}""");

        var result = await dispatcher.DispatchAsync(request);

        Assert.IsFalse(result.Success);
        Assert.AreEqual("unsupported_action", result.Error?.Code);
    }

    [TestMethod]
    public async Task TestReachabilityReturnsCanonicalConnectableDto()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());
        var request = Request("testReachability", "{}");

        var result = await dispatcher.DispatchAsync(request);

        Assert.IsTrue(result.Success);
        var json = JsonSerializer.SerializeToElement(result.Payload);
        Assert.IsTrue(json.GetProperty("connectable").GetBoolean());
        Assert.IsFalse(json.TryGetProperty("reachable", out _));
    }

    [TestMethod]
    public async Task ExecuteConsoleCommandReturnsCanonicalCommandOutputDto()
    {
        var dispatcher = new TakaroRequestDispatcher(new FakeAdapter());
        var request = Request("executeConsoleCommand", """{"command":"status"}""");

        var result = await dispatcher.DispatchAsync(request);

        Assert.IsTrue(result.Success);
        var json = JsonSerializer.SerializeToElement(result.Payload);
        Assert.IsTrue(json.GetProperty("success").GetBoolean());
        Assert.AreEqual("Executed: status", json.GetProperty("rawResult").GetString());
    }

    private static TakaroRequest Request(string action, string args)
    {
        using var json = JsonDocument.Parse(args);
        return new TakaroRequest("req", action, json.RootElement.Clone());
    }

    private sealed class FakeAdapter : ICs2TakaroAdapter
    {
        public string? LastMessage { get; private set; }
        public string? LastRecipient { get; private set; }
        public string? LastSender { get; private set; }
        public string? LastIdentifier { get; private set; }

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new { connectable = true, reason = (string?)null }));

        public Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<TakaroPlayer>>([]);

        public Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
            Task.FromResult<TakaroPlayer?>(null);

        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipient, string? sender, CancellationToken cancellationToken = default)
        {
            LastMessage = message;
            LastRecipient = recipient;
            LastSender = sender;
            return Task.FromResult(TakaroActionResult.Ok());
        }

        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) =>
            Task.FromResult(TakaroActionResult.Ok(new { success = true, rawResult = $"Executed: {command}" }));

        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) { LastIdentifier = identifier; return Unsupported(); }
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) { LastIdentifier = identifier; return Unsupported(); }
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string code, int amount, CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, string? expiresAt, CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => Unsupported();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => Unsupported();
        private static Task<TakaroActionResult> Unsupported() => Task.FromResult(TakaroActionResult.Fail("unsupported", "fake"));
    }
}
