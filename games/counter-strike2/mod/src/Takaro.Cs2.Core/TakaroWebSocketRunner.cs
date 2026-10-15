using System.Net.WebSockets;
using System.Text;
using System.Collections.Concurrent;
using System.Text.Json;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core;

public sealed class TakaroWebSocketRunner : IDisposable
{
    private readonly ConnectorConfig config;
    private readonly TakaroRequestDispatcher dispatcher;
    private readonly Action<string> log;
    private readonly CancellationTokenSource shutdown = new();
    private readonly SemaphoreSlim sendLock = new(1, 1);
    private readonly SemaphoreSlim eventLock = new(1, 1);
    private readonly ConcurrentQueue<string> pendingEvents = new();
    private volatile bool identified;
    private ClientWebSocket? socket;
    private Task? runLoop;

    public TakaroWebSocketRunner(ConnectorConfig config, ICs2TakaroAdapter adapter, Action<string>? log = null)
    {
        this.config = config;
        dispatcher = new TakaroRequestDispatcher(adapter);
        this.log = log ?? (_ => { });
    }

    public bool IsRunning => runLoop is { IsCompleted: false };

    public Task StartAsync()
    {
        runLoop ??= Task.Run(() => RunAsync(shutdown.Token));
        return Task.CompletedTask;
    }

    public async Task SendGameEventAsync(string eventType, object data, CancellationToken cancellationToken = default)
    {
        if (pendingEvents.Count >= 10000)
        {
            log("Takaro CS2 event queue is full; event rejected.");
            return;
        }
        pendingEvents.Enqueue(TakaroProtocol.CreateGameEvent(eventType, data));
        await FlushEventsAsync(cancellationToken);
    }

    public void Dispose()
    {
        shutdown.Cancel();
        socket?.Dispose();
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        var attempt = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                using var client = new ClientWebSocket();
                socket = client;
                identified = false;
                await client.ConnectAsync(new Uri(config.TakaroWsUrl), cancellationToken);
                await SendAsync(client, TakaroProtocol.CreateIdentify(config), cancellationToken);
                log("Takaro CS2 identify sent.");
                attempt = 0;
                await ReceiveLoopAsync(client, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                if (cancellationToken.IsCancellationRequested) return;
                attempt++;
                var delay = TimeSpan.FromMilliseconds(Math.Min(60000, 1000 * Math.Pow(2, attempt)));
                log($"Takaro CS2 WebSocket reconnect after error: {ex.Message}");
                await Task.Delay(delay, cancellationToken);
            }
            finally
            {
                socket = null;
                identified = false;
            }
        }
    }

    private async Task ReceiveLoopAsync(ClientWebSocket activeSocket, CancellationToken cancellationToken)
    {
        while (activeSocket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
        {
            var message = await ReceiveMessageAsync(activeSocket, cancellationToken);
            if (message is null)
            {
                return;
            }

            string? type;
            try
            {
                using var doc = JsonDocument.Parse(message);
                type = doc.RootElement.TryGetProperty("type", out var member) ? member.GetString() : null;
            }
            catch (JsonException)
            {
                log("Takaro CS2 received malformed JSON frame.");
                continue;
            }

            if (type == "ping")
            {
                await SendAsync(activeSocket, """{"type":"pong"}""", cancellationToken);
                continue;
            }

            if (type == "connected")
            {
                log("Takaro CS2 WebSocket acknowledged by Takaro.");
                continue;
            }

            if (type == "identifyResponse")
            {
                if (LogIdentifyResponse(message))
                {
                    identified = true;
                    await FlushEventsAsync(cancellationToken);
                }
                continue;
            }

            if (type == "error")
            {
                LogTakaroError(message);
                continue;
            }

            if (type != "request")
            {
                continue;
            }

            try
            {
                var request = TakaroProtocol.ParseRequest(message);
                log($"Takaro CS2 request received: action={request.Action}, requestId={request.RequestId}.");
                var response = await dispatcher.DispatchAsync(request, cancellationToken);
                await SendAsync(activeSocket, TakaroProtocol.CreateResponse(request.RequestId, response), cancellationToken);
                log($"Takaro CS2 response sent: action={request.Action}, success={response.Success}.");
            }
            catch (Exception ex)
            {
                log($"Takaro CS2 could not handle request message: {ex.Message}");
            }
        }
    }

    private async Task<string?> ReceiveMessageAsync(ClientWebSocket activeSocket, CancellationToken cancellationToken)
    {
        var buffer = new byte[32 * 1024];
        using var message = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await activeSocket.ReceiveAsync(buffer, cancellationToken);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                return null;
            }

            message.Write(buffer, 0, result.Count);
        }
        while (!result.EndOfMessage);

        return Encoding.UTF8.GetString(message.ToArray());
    }

    private async Task<bool> SendAsync(ClientWebSocket activeSocket, string json, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(json);
        await sendLock.WaitAsync(cancellationToken);
        try
        {
            if (activeSocket.State == WebSocketState.Open)
            {
                await activeSocket.SendAsync(bytes, WebSocketMessageType.Text, true, cancellationToken);
                return true;
            }
            return false;
        }
        finally
        {
            sendLock.Release();
        }
    }

    private bool LogIdentifyResponse(string message)
    {
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(message);
            var payload = doc.RootElement.TryGetProperty("payload", out var payloadElement)
                ? payloadElement
                : default;

            if (payload.ValueKind == System.Text.Json.JsonValueKind.Object
                && payload.TryGetProperty("error", out var error)
                && error.ValueKind != System.Text.Json.JsonValueKind.Null
                && error.ValueKind != System.Text.Json.JsonValueKind.Undefined)
            {
                log("Takaro CS2 identification failed; inspect Takaro server diagnostics.");
                return false;
            }

            if (payload.ValueKind == System.Text.Json.JsonValueKind.Object
                && payload.TryGetProperty("gameServerId", out var gameServerId)
                && gameServerId.ValueKind == System.Text.Json.JsonValueKind.String)
            {
                log($"Takaro CS2 identified as gameServerId={gameServerId.GetString()}.");
                return true;
            }

            log("Takaro CS2 identifyResponse received without gameServerId.");
            return false;
        }
        catch (Exception ex)
        {
            log($"Takaro CS2 could not parse identifyResponse: {ex.Message}");
            return false;
        }
    }

    private async Task FlushEventsAsync(CancellationToken cancellationToken)
    {
        var activeSocket = socket;
        if (!identified || activeSocket is null || activeSocket.State != WebSocketState.Open) return;
        await eventLock.WaitAsync(cancellationToken);
        try
        {
            while (identified && activeSocket.State == WebSocketState.Open && pendingEvents.TryPeek(out var frame))
            {
                if (!await SendAsync(activeSocket, frame, cancellationToken)) break;
                pendingEvents.TryDequeue(out _);
            }
        }
        finally { eventLock.Release(); }
    }

    private void LogTakaroError(string message)
    {
        log("Takaro CS2 WebSocket error message received; inspect Takaro server diagnostics.");
    }
}
