using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

/// <summary>
/// The runner against a fake Takaro that negotiates version 0 or 1: stream positions, resend
/// after a reconnect, eventAck, refusing undeclared actions, and v1 identify rejections.
/// </summary>
[TestClass]
public sealed class RunnerProtocolV1Tests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [TestMethod]
    public async Task VersionOneEventsCarryAStreamPositionAndAreResentUnderItAfterAReconnect()
    {
        using var takaro = new FakeTakaro(chosenVersion: 1);
        using var runner = Runner(takaro);
        await runner.StartAsync();
        await Until(() => takaro.Identifies >= 1 && takaro.Connected);

        await runner.SendGameEventAsync("chat-message", new { msg = "one" });
        await runner.SendGameEventAsync("chat-message", new { msg = "two" });
        await Until(() => takaro.Events.Count >= 2);
        var first = takaro.Events.ToArray();
        var streamId = first[0].StreamId;
        Assert.IsNotNull(streamId);
        CollectionAssert.AreEqual(new long?[] { 1, 2 }, first.Select(e => e.Seq).ToArray());
        Assert.AreEqual(streamId, first[1].StreamId);

        await takaro.DropAsync();
        await Until(() => takaro.Events.Count >= 4);
        var resent = takaro.Events.Skip(2).ToArray();
        CollectionAssert.AreEqual(new long?[] { 1, 2 }, resent.Select(e => e.Seq).ToArray(), "unacknowledged events are resent in seq order");
        Assert.IsTrue(resent.All(e => e.StreamId == streamId), "the stream outlives the connection");
        CollectionAssert.AreEqual(new[] { "one", "two" }, resent.Select(e => e.Message).ToArray());
    }

    [TestMethod]
    public async Task EventAckReleasesEventsSoTheyAreNotResent()
    {
        using var takaro = new FakeTakaro(chosenVersion: 1);
        using var runner = Runner(takaro);
        await runner.StartAsync();
        await Until(() => takaro.Identifies >= 1 && takaro.Connected);

        await runner.SendGameEventAsync("chat-message", new { msg = "acked" });
        await Until(() => takaro.Events.Count >= 1);
        await takaro.SendAsync("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"" + takaro.Events.First().StreamId + "\",\"upTo\":1},\"requestId\":\"a\"}");
        await Until(() => takaro.LogLines.Any(l => l.Contains("up to seq 1", StringComparison.Ordinal)));
        await runner.SendGameEventAsync("chat-message", new { msg = "not acked" });
        await Until(() => takaro.Events.Count >= 2);

        await takaro.DropAsync();
        await Until(() => takaro.Events.Count >= 3);
        await Task.Delay(300);
        var resent = takaro.Events.Skip(2).ToArray();
        Assert.AreEqual(1, resent.Length);
        Assert.AreEqual("not acked", resent[0].Message);
        Assert.AreEqual(2L, resent[0].Seq);
    }

    [TestMethod]
    public async Task VersionZeroEventsKeepTheOldShape()
    {
        using var takaro = new FakeTakaro(chosenVersion: null);
        using var runner = Runner(takaro);
        await runner.StartAsync();
        await Until(() => takaro.Identifies >= 1 && takaro.Connected);

        await runner.SendGameEventAsync("chat-message", new { msg = "old" });
        await Until(() => takaro.Events.Count >= 1);

        var gameEvent = takaro.Events.First();
        Assert.IsNull(gameEvent.StreamId);
        Assert.IsNull(gameEvent.Seq);
    }

    [TestMethod]
    public async Task VersionOneRefusesUndeclaredActionsAndAnswersDeclaredOnes()
    {
        using var takaro = new FakeTakaro(chosenVersion: 1);
        var adapter = new RecordingAdapter();
        using var runner = Runner(takaro, adapter);
        await runner.StartAsync();
        await Until(() => takaro.Identifies >= 1 && takaro.Connected);

        await takaro.SendAsync("""{"type":"request","requestId":"r-1","payload":{"action":"getPlayer","args":"{\"gameId\":\"Steam_1\"}"}}""");
        await takaro.SendAsync("""{"type":"request","requestId":"r-2","payload":{"action":"sendMessage","args":"{\"message\":\"hi\"}"}}""");
        await Until(() => takaro.Responses.ContainsKey("r-1") && takaro.Responses.ContainsKey("r-2"));

        using var refused = JsonDocument.Parse(takaro.Responses["r-1"]);
        Assert.AreEqual("unsupported", refused.RootElement.GetProperty("error").GetProperty("code").GetString());
        Assert.AreEqual(0, adapter.GetPlayerCalls, "an undeclared action never reaches the game");

        using var answered = JsonDocument.Parse(takaro.Responses["r-2"]);
        Assert.AreEqual(JsonValueKind.Null, answered.RootElement.GetProperty("payload").ValueKind);
        Assert.IsFalse(answered.RootElement.TryGetProperty("error", out _));
        Assert.AreEqual("hi", adapter.LastMessage);
    }

    [TestMethod]
    public async Task VersionZeroStillRunsActionsVersionOneWouldNotDeclare()
    {
        using var takaro = new FakeTakaro(chosenVersion: 0);
        var adapter = new RecordingAdapter();
        using var runner = Runner(takaro, adapter);
        await runner.StartAsync();
        await Until(() => takaro.Identifies >= 1 && takaro.Connected);

        await takaro.SendAsync("""{"type":"request","requestId":"r-1","payload":{"action":"getPlayer","args":"{\"gameId\":\"Steam_1\"}"}}""");
        await Until(() => takaro.Responses.ContainsKey("r-1"));

        Assert.AreEqual(1, adapter.GetPlayerCalls);
    }

    [TestMethod]
    public async Task AVersionOneRejectionBacksOffInsteadOfReconnectingAtOnce()
    {
        using var takaro = new FakeTakaro(chosenVersion: 1, rejectWith: "invalid_args");
        var logs = new ConcurrentQueue<string>();
        using var runner = new TakaroWebSocketRunner(
            Config(takaro), new RecordingAdapter(), logs.Enqueue, null, logs.Enqueue, "cfg", TimeSpan.FromSeconds(1));
        await runner.StartAsync();

        await Until(() => logs.Any(l => l.Contains("Takaro rejected this server", StringComparison.Ordinal)));
        StringAssert.Contains(logs.First(l => l.Contains("Takaro rejected this server", StringComparison.Ordinal)), "invalid_args");
        await Task.Delay(1000);
        Assert.AreEqual(1, takaro.Identifies, "the close that follows a v1 rejection must not skip the backoff");
    }

    private static TakaroWebSocketRunner Runner(FakeTakaro takaro, IValheimTakaroAdapter? adapter = null) =>
        new(Config(takaro), adapter ?? new RecordingAdapter(), takaro.Log, null, takaro.Log, "cfg");

    private static ConnectorConfig Config(FakeTakaro takaro) =>
        ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "good",
            ["identityToken"] = "id-1",
            ["takaroWsUrl"] = takaro.Url
        });

    private static async Task Until(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + Wait;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                Assert.Fail("Timed out waiting for the runner.");
            }

            await Task.Delay(25);
        }
    }

    private sealed record ReceivedEvent(string? StreamId, long? Seq, string? Message);

    private sealed class FakeTakaro : IDisposable
    {
        private readonly HttpListener listener = new();
        private readonly CancellationTokenSource stop = new();
        private readonly int? chosenVersion;
        private readonly string? rejectWith;
        private readonly SemaphoreSlim sendLock = new(1, 1);
        private readonly ConcurrentQueue<string> logs = new();
        private WebSocket? current;
        private int identifies;

        public FakeTakaro(int? chosenVersion, string? rejectWith = null)
        {
            this.chosenVersion = chosenVersion;
            this.rejectWith = rejectWith;
            var probe = new TcpListener(IPAddress.Loopback, 0);
            probe.Start();
            var port = ((IPEndPoint)probe.LocalEndpoint).Port;
            probe.Stop();
            listener.Prefixes.Add($"http://127.0.0.1:{port}/");
            listener.Start();
            Url = $"ws://127.0.0.1:{port}/";
            _ = Task.Run(AcceptAsync);
        }

        public string Url { get; }

        public int Identifies => Volatile.Read(ref identifies);

        public bool Connected => current is { State: WebSocketState.Open };

        public ConcurrentQueue<ReceivedEvent> Events { get; } = new();

        public ConcurrentDictionary<string, string> Responses { get; } = new();

        public void Log(string line) => logs.Enqueue(line);

        public IEnumerable<string> LogLines => logs;

        public async Task DropAsync()
        {
            await sendLock.WaitAsync();
            try
            {
                await current!.CloseOutputAsync(WebSocketCloseStatus.EndpointUnavailable, "restart", stop.Token);
            }
            finally
            {
                sendLock.Release();
            }
        }

        public async Task SendAsync(string frame)
        {
            await sendLock.WaitAsync();
            try
            {
                await current!.SendAsync(Encoding.UTF8.GetBytes(frame), WebSocketMessageType.Text, true, stop.Token);
            }
            finally
            {
                sendLock.Release();
            }
        }

        public void Dispose()
        {
            stop.Cancel();
            listener.Close();
        }

        private async Task AcceptAsync()
        {
            while (!stop.IsCancellationRequested)
            {
                HttpListenerContext context;
                try
                {
                    context = await listener.GetContextAsync();
                }
                catch (Exception)
                {
                    return;
                }

                _ = Task.Run(() => ServeAsync(context));
            }
        }

        private async Task ServeAsync(HttpListenerContext context)
        {
            try
            {
                var socket = (await context.AcceptWebSocketAsync(null)).WebSocket;
                current = socket;
                var buffer = new byte[64 * 1024];
                while (socket.State == WebSocketState.Open && !stop.IsCancellationRequested)
                {
                    var result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), stop.Token);
                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        break;
                    }

                    var text = Encoding.UTF8.GetString(buffer, 0, result.Count);
                    using var frame = JsonDocument.Parse(text);
                    var root = frame.RootElement;
                    switch (root.GetProperty("type").GetString())
                    {
                        case "ping":
                            await SendAsync("""{"type":"pong"}""");
                            break;
                        case "identify":
                            Interlocked.Increment(ref identifies);
                            if (rejectWith is not null)
                            {
                                await SendAsync("{\"type\":\"identifyResponse\",\"payload\":null,\"error\":{\"code\":\"" + rejectWith + "\",\"message\":\"bad manifest\"}}");
                                await socket.CloseAsync(WebSocketCloseStatus.PolicyViolation, rejectWith, stop.Token);
                                return;
                            }

                            var version = chosenVersion is null ? "" : ",\"protocolVersion\":" + chosenVersion;
                            await SendAsync("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-1\"" + version + "}}");
                            break;
                        case "gameEvent":
                            var payload = root.GetProperty("payload");
                            Events.Enqueue(new ReceivedEvent(
                                payload.TryGetProperty("streamId", out var stream) ? stream.GetString() : null,
                                payload.TryGetProperty("seq", out var seq) ? seq.GetInt64() : null,
                                payload.GetProperty("data").TryGetProperty("msg", out var msg) ? msg.GetString() : null));
                            break;
                        case "response":
                            Responses[root.GetProperty("requestId").GetString()!] = text;
                            break;
                    }
                }
            }
            catch (Exception)
            {
            }
        }
    }

    private sealed class RecordingAdapter : IValheimTakaroAdapter
    {
        private static Task<TakaroActionResult> Unused() => Task.FromResult(TakaroActionResult.Error("unused", "unused"));

        public int GetPlayerCalls;
        public string? LastMessage;

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => Task.FromResult(TakaroActionResult.Ok(new { connectable = true }));
        public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) => Task.FromResult(TakaroActionResult.Ok(Array.Empty<TakaroPlayer>()));
        public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref GetPlayerCalls);
            return Task.FromResult(TakaroActionResult.Ok(null));
        }

        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, string? senderNameOverride, CancellationToken cancellationToken = default)
        {
            LastMessage = message;
            return Task.FromResult(TakaroActionResult.Ok(null));
        }

        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetMapTileAsync(int x, int y, int z, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => Unused();
    }
}
