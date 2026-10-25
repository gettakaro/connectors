using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class WebSocketPeerTests
{
    [TestMethod]
    public async Task IdentifiesRespondsByRequestIdAndReplaysQueuedEventAfterReconnect()
    {
        var portSocket = new TcpListener(IPAddress.Loopback, 0);
        portSocket.Start();
        var port = ((IPEndPoint)portSocket.LocalEndpoint).Port;
        portSocket.Stop();
        using var listener = new HttpListener();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        listener.Start();
        using var runner = new TakaroWebSocketRunner(new ConnectorConfig
        {
            TakaroWsUrl = $"ws://127.0.0.1:{port}/",
            IdentityToken = "test-identity"
        }, new PeerAdapter());
        await runner.StartAsync();

        using var first = (await (await listener.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(10))).AcceptWebSocketAsync(null)).WebSocket;
        Assert.AreEqual("identify", Type(await Receive(first)));
        await Send(first, """{"type":"identifyResponse","payload":{"gameServerId":"test"}}""");
        await Send(first, """{"type":"ping"}""");
        Assert.AreEqual("pong", Type(await Receive(first)));
        await Send(first, """{"type":"request","requestId":"one","payload":{"action":"getPlayers","args":[]}}""");
        await Send(first, """{"type":"request","requestId":"two","payload":{"action":"getPlayers","args":"{}"}}""");
        Assert.AreEqual("one", JsonDocument.Parse(await Receive(first)).RootElement.GetProperty("requestId").GetString());
        Assert.AreEqual("two", JsonDocument.Parse(await Receive(first)).RootElement.GetProperty("requestId").GetString());
        await first.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "test", CancellationToken.None);

        using var second = (await (await listener.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(10))).AcceptWebSocketAsync(null)).WebSocket;
        Assert.AreEqual("identify", Type(await Receive(second)));
        await runner.SendGameEventAsync("log", new { level = "info", msg = "queued" });
        await Send(second, """{"type":"identifyResponse","payload":{"gameServerId":"test"}}""");
        var delivered = JsonDocument.Parse(await Receive(second)).RootElement;
        Assert.AreEqual("gameEvent", delivered.GetProperty("type").GetString());
        Assert.AreEqual("queued", delivered.GetProperty("payload").GetProperty("data").GetProperty("msg").GetString());
    }

    private static string Type(string json) => JsonDocument.Parse(json).RootElement.GetProperty("type").GetString()!;

    private static async Task Send(WebSocket socket, string json) =>
        await socket.SendAsync(Encoding.UTF8.GetBytes(json), WebSocketMessageType.Text, true, CancellationToken.None);

    private static async Task<string> Receive(WebSocket socket)
    {
        var bytes = new byte[8192];
        var result = await socket.ReceiveAsync(bytes, new CancellationTokenSource(TimeSpan.FromSeconds(10)).Token);
        return Encoding.UTF8.GetString(bytes, 0, result.Count);
    }

    private sealed class PeerAdapter : ICs2TakaroAdapter
    {
        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default) => Task.FromResult<IReadOnlyList<TakaroPlayer>>([]);
        public Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Task.FromResult<TakaroPlayer?>(null);
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipient, string? sender, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string code, int amount, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetMapTileAsync(int x, int y, int zoom, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, string? expiresAt, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => Ok();
        private static Task<TakaroActionResult> Ok() => Task.FromResult(TakaroActionResult.Ok());
    }
}
