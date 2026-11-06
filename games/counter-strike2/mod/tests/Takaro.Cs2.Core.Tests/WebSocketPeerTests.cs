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
        await Send(first, """{"type":"identifyResponse","payload":{"gameServerId":"test","protocolVersion":1}}""");
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
        await Send(second, """{"type":"identifyResponse","payload":{"gameServerId":"test","protocolVersion":1}}""");
        var delivered = JsonDocument.Parse(await Receive(second)).RootElement;
        Assert.AreEqual("gameEvent", delivered.GetProperty("type").GetString());
        var payload = delivered.GetProperty("payload");
        Assert.AreEqual("queued", payload.GetProperty("data").GetProperty("msg").GetString());
        Assert.AreEqual(1, payload.GetProperty("seq").GetInt64());
        var stream = payload.GetProperty("streamId").GetString();

        // Unacknowledged events are resent in order after the next identify; acknowledged ones are not.
        await runner.SendGameEventAsync("log", new { level = "info", msg = "second" });
        Assert.AreEqual(2, JsonDocument.Parse(await Receive(second)).RootElement.GetProperty("payload").GetProperty("seq").GetInt64());
        await Send(second, JsonSerializer.Serialize(new { type = "eventAck", payload = new { streamId = stream, upTo = 1 } }));
        await Send(second, """{"type":"ping"}""");
        Assert.AreEqual("pong", Type(await Receive(second)));
        await second.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "test", CancellationToken.None);

        using var third = (await (await listener.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(10))).AcceptWebSocketAsync(null)).WebSocket;
        Assert.AreEqual("identify", Type(await Receive(third)));
        await Send(third, """{"type":"identifyResponse","payload":{"gameServerId":"test","protocolVersion":1}}""");
        var resent = JsonDocument.Parse(await Receive(third)).RootElement.GetProperty("payload");
        Assert.AreEqual(2, resent.GetProperty("seq").GetInt64());
        Assert.AreEqual(stream, resent.GetProperty("streamId").GetString());
    }

    [TestMethod]
    public async Task ResendsTheManifestWhenItChangedDuringIdentify()
    {
        var portSocket = new TcpListener(IPAddress.Loopback, 0);
        portSocket.Start();
        var port = ((IPEndPoint)portSocket.LocalEndpoint).Port;
        portSocket.Stop();
        using var listener = new HttpListener();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        listener.Start();
        var adapter = new PeerAdapter();
        using var runner = new TakaroWebSocketRunner(new ConnectorConfig { TakaroWsUrl = $"ws://127.0.0.1:{port}/", IdentityToken = "test-identity" }, adapter);
        await runner.StartAsync();

        using var peer = (await (await listener.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(10))).AcceptWebSocketAsync(null)).WebSocket;
        Assert.AreEqual("identify", Type(await Receive(peer)));
        adapter.Capabilities = [new { name = "players.list" }, new { name = "map" }];
        await Send(peer, """{"type":"identifyResponse","payload":{"gameServerId":"test","protocolVersion":1}}""");
        var replaced = JsonDocument.Parse(await Receive(peer)).RootElement;
        Assert.AreEqual("capabilities", replaced.GetProperty("type").GetString());
        Assert.AreEqual(2, replaced.GetProperty("payload").GetProperty("capabilities").GetArrayLength());
    }

    private static (HttpListener Listener, int Port) Listen()
    {
        var portSocket = new TcpListener(IPAddress.Loopback, 0);
        portSocket.Start();
        var port = ((IPEndPoint)portSocket.LocalEndpoint).Port;
        portSocket.Stop();
        var listener = new HttpListener();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        listener.Start();
        return (listener, port);
    }

    private static async Task<WebSocket> Accept(HttpListener listener) =>
        (await (await listener.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(10))).AcceptWebSocketAsync(null)).WebSocket;

    private const string Identified = """{"type":"identifyResponse","payload":{"gameServerId":"test","protocolVersion":1}}""";

    [TestMethod]
    public async Task AnAckBeyondWhatWasSentKeepsUnsentEvents()
    {
        var (listener, port) = Listen();
        using var _ = listener;
        using var runner = new TakaroWebSocketRunner(new ConnectorConfig { TakaroWsUrl = $"ws://127.0.0.1:{port}/", IdentityToken = "test-identity" }, new PeerAdapter());
        await runner.StartAsync();
        using var first = await Accept(listener);
        await Receive(first);
        await runner.SendGameEventAsync("log", new { msg = "unsent" });
        await Send(first, Identified);
        var stream = JsonDocument.Parse(await Receive(first)).RootElement.GetProperty("payload").GetProperty("streamId").GetString();
        await Send(first, JsonSerializer.Serialize(new { type = "eventAck", payload = new { streamId = stream, upTo = 999 } }));
        await Send(first, """{"type":"ping"}""");
        await Receive(first);
        await runner.SendGameEventAsync("log", new { msg = "later" });
        Assert.AreEqual(2, JsonDocument.Parse(await Receive(first)).RootElement.GetProperty("payload").GetProperty("seq").GetInt64());
        await first.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "test", CancellationToken.None);

        using var second = await Accept(listener);
        await Receive(second);
        await Send(second, Identified);
        Assert.AreEqual(2, JsonDocument.Parse(await Receive(second)).RootElement.GetProperty("payload").GetProperty("seq").GetInt64());
    }

    [TestMethod]
    public async Task ReconnectsAndReplaysWhenAnOpenSocketStopsAcknowledging()
    {
        var (listener, port) = Listen();
        using var _ = listener;
        using var runner = new TakaroWebSocketRunner(new ConnectorConfig { TakaroWsUrl = $"ws://127.0.0.1:{port}/", IdentityToken = "test-identity" }, new PeerAdapter(), ackTimeout: TimeSpan.FromMilliseconds(400));
        await runner.StartAsync();
        using var first = await Accept(listener);
        await Receive(first);
        await Send(first, Identified);
        await runner.SendGameEventAsync("log", new { msg = "silent" });
        await Receive(first);

        using var second = await Accept(listener);
        await Receive(second);
        await Send(second, Identified);
        Assert.AreEqual(1, JsonDocument.Parse(await Receive(second)).RootElement.GetProperty("payload").GetProperty("seq").GetInt64());
    }

    [TestMethod]
    public async Task AStalledRequestIsAnsweredWithAnError()
    {
        var (listener, port) = Listen();
        using var _ = listener;
        using var runner = new TakaroWebSocketRunner(new ConnectorConfig { TakaroWsUrl = $"ws://127.0.0.1:{port}/", IdentityToken = "test-identity" }, new PeerAdapter { StallPlayers = true }, requestTimeout: TimeSpan.FromMilliseconds(200));
        await runner.StartAsync();
        using var first = await Accept(listener);
        await Receive(first);
        await Send(first, Identified);
        await Send(first, """{"type":"request","requestId":"stuck","payload":{"action":"getPlayers","args":"{}"}}""");
        var response = JsonDocument.Parse(await Receive(first)).RootElement;
        Assert.AreEqual("stuck", response.GetProperty("requestId").GetString());
        Assert.AreEqual("internal", response.GetProperty("error").GetProperty("code").GetString());
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
        public Task<object?> GetPlayerProgressionAsync(string identifier, CancellationToken cancellationToken = default) => Task.FromResult<object?>(null);
        public IReadOnlyList<object> Capabilities { get; set; } = [new { name = "players.list" }];
        public IReadOnlyList<object> GetCapabilities() => Capabilities;
        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => Ok();
        public bool StallPlayers { get; init; }
        public async Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default)
        {
            if (StallPlayers) await Task.Delay(Timeout.Infinite, cancellationToken);
            return [];
        }
        public Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Task.FromResult<TakaroPlayer?>(null);
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipient, string? sender, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string code, int amount, CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Ok();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => Ok();
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
