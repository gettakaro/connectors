using System.Net.WebSockets;
using System.Text;
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
    private const int MaxBufferedEvents = 10000;
    private readonly string streamId = Guid.NewGuid().ToString();
    private readonly object eventGate = new();
    private readonly Queue<(long Seq, string Type, string Frame)> bufferedEvents = new();
    private readonly ICs2TakaroAdapter adapter;
    private long nextSeq = 1;
    private long lastSentSeq;
    private long highestSentSeq;
    private long connectionGeneration;
    private DateTimeOffset? unackedSince;
    private readonly TimeSpan ackTimeout;
    private readonly TimeSpan requestTimeout;
    private readonly SemaphoreSlim manifestLock = new(1, 1);
    private string identifiedManifest = "[]";
    private volatile bool identified;
    private ClientWebSocket? socket;
    private Task? runLoop;

    public TakaroWebSocketRunner(ConnectorConfig config, ICs2TakaroAdapter adapter, Action<string>? log = null,
        TimeSpan? ackTimeout = null, TimeSpan? requestTimeout = null)
    {
        this.ackTimeout = ackTimeout ?? TimeSpan.FromSeconds(30);
        // Below Takaro's 10 second request timeout, so a stalled game thread cannot block pings for long.
        this.requestTimeout = requestTimeout ?? TimeSpan.FromSeconds(9);
        this.config = config;
        this.adapter = adapter;
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
        lock (eventGate)
        {
            if (bufferedEvents.Count >= MaxBufferedEvents)
            {
                bufferedEvents.Dequeue();
                log("Takaro CS2 event buffer is full; oldest event dropped.");
            }
            var seq = nextSeq++;
            bufferedEvents.Enqueue((seq, eventType, TakaroProtocol.CreateGameEvent(eventType, data, streamId, seq)));
        }
        await FlushEventsAsync(cancellationToken);
    }

    /// <summary>Replaces the manifest on the live connection, for example after a map change.</summary>
    public async Task UpdateCapabilitiesAsync(CancellationToken cancellationToken = default)
    {
        var activeSocket = socket;
        if (!identified || activeSocket is null) return;
        await manifestLock.WaitAsync(cancellationToken);
        try
        {
            var capabilities = adapter.GetCapabilities();
            await SendAsync(activeSocket, TakaroProtocol.CreateCapabilities(Guid.NewGuid().ToString(), capabilities), cancellationToken);
            log($"Takaro CS2 capabilities replaced: {capabilities.Count} entries.");
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            log($"Takaro CS2 could not replace capabilities: {ex.Message}");
        }
        finally { manifestLock.Release(); }
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
            var wasIdentified = false;
            try
            {
                using var client = new ClientWebSocket();
                socket = client;
                identified = false;
                lock (eventGate) unackedSince = null;
                await client.ConnectAsync(new Uri(config.TakaroWsUrl), cancellationToken);
                var capabilities = adapter.GetCapabilities();
                identifiedManifest = JsonSerializer.Serialize(capabilities, TakaroProtocol.JsonOptions);
                await SendAsync(client, TakaroProtocol.CreateIdentify(config, capabilities), cancellationToken);
                log("Takaro CS2 identify sent.");
                using var connection = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                _ = WatchAcknowledgementsAsync(client, connection.Token);
                wasIdentified = await ReceiveLoopAsync(client, cancellationToken);
                connection.Cancel();
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                if (cancellationToken.IsCancellationRequested) return;
                log($"Takaro CS2 WebSocket reconnect after error: {ex.Message}");
            }
            finally
            {
                socket = null;
                identified = false;
            }

            attempt = wasIdentified ? 1 : attempt + 1;
            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(60000, 1000 * Math.Pow(2, attempt))), cancellationToken);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    private async Task<bool> ReceiveLoopAsync(ClientWebSocket activeSocket, CancellationToken cancellationToken)
    {
        var wasIdentified = false;
        while (activeSocket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
        {
            var message = await ReceiveMessageAsync(activeSocket, cancellationToken);
            if (message is null)
            {
                return wasIdentified;
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
                    wasIdentified = true;
                    lock (eventGate)
                    {
                        connectionGeneration++;
                        lastSentSeq = 0;
                        unackedSince = null;
                    }
                    identified = true;
                    // A map that started while identify was in flight is not in the manifest we sent.
                    if (JsonSerializer.Serialize(adapter.GetCapabilities(), TakaroProtocol.JsonOptions) != identifiedManifest)
                        await UpdateCapabilitiesAsync(cancellationToken);
                    await FlushEventsAsync(cancellationToken);
                }
                continue;
            }

            if (type == "eventAck")
            {
                HandleEventAck(message);
                continue;
            }

            if (type is "eventError" or "capabilitiesResponse")
            {
                LogFrameError(type, message);
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
                // Cancelling also stops a queued game action, so a late effect cannot follow a reported failure.
                using var requestWindow = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                requestWindow.CancelAfter(requestTimeout);
                var response = await dispatcher.DispatchAsync(request, requestWindow.Token);
                await SendAsync(activeSocket, TakaroProtocol.CreateActionResponse(request.RequestId, request.Action, response), cancellationToken);
                log($"Takaro CS2 response sent: action={request.Action}, success={response.Success}.");
            }
            catch (Exception ex)
            {
                log($"Takaro CS2 could not handle request message: {ex.Message}");
            }
        }
        return wasIdentified;
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
            using var doc = JsonDocument.Parse(message);
            var root = doc.RootElement;
            var payload = root.TryGetProperty("payload", out var payloadElement) ? payloadElement : default;

            if (HasError(root) || (payload.ValueKind == JsonValueKind.Object && HasError(payload)))
            {
                log($"Takaro CS2 identification failed: {Redact(ErrorText(HasError(root) ? root : payload))}");
                return false;
            }

            if (payload.ValueKind != JsonValueKind.Object
                || !payload.TryGetProperty("gameServerId", out var gameServerId)
                || gameServerId.ValueKind != JsonValueKind.String)
            {
                log("Takaro CS2 identifyResponse received without gameServerId.");
                return false;
            }

            if (!payload.TryGetProperty("protocolVersion", out var version) || !version.TryGetInt32(out var negotiated) || negotiated != 1)
            {
                throw new InvalidOperationException("This Takaro server does not support protocol version 1.");
            }

            log($"Takaro CS2 identified as gameServerId={gameServerId.GetString()}.");
            return true;
        }
        catch (JsonException ex)
        {
            log($"Takaro CS2 could not parse identifyResponse: {ex.Message}");
            return false;
        }
    }

    private static bool HasError(JsonElement element) =>
        element.TryGetProperty("error", out var error) && error.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined or JsonValueKind.False);

    private static string ErrorText(JsonElement element)
    {
        var error = element.GetProperty("error");
        if (error.ValueKind != JsonValueKind.Object) return error.ToString();
        var code = error.TryGetProperty("code", out var codeValue) ? codeValue.ToString() : "unknown";
        var detail = error.TryGetProperty("message", out var messageValue) ? messageValue.ToString() : "No message supplied.";
        return $"code={code}, message={detail}";
    }

    private string Redact(string text)
    {
        foreach (var secret in new[] { config.RegistrationToken, config.IdentityToken })
        {
            if (!string.IsNullOrEmpty(secret)) text = text.Replace(secret, "[redacted]", StringComparison.Ordinal);
        }
        return text[..Math.Min(text.Length, 300)];
    }

    private void HandleEventAck(string message)
    {
        using var doc = JsonDocument.Parse(message);
        if (!doc.RootElement.TryGetProperty("payload", out var payload) || payload.ValueKind != JsonValueKind.Object) return;
        if (!payload.TryGetProperty("streamId", out var stream) || stream.GetString() != streamId) return;
        if (!payload.TryGetProperty("upTo", out var upTo) || !upTo.TryGetInt64(out var acknowledged)) return;
        lock (eventGate)
        {
            var upToSent = Math.Min(acknowledged, highestSentSeq);
            var advanced = false;
            while (bufferedEvents.TryPeek(out var oldest) && oldest.Seq <= upToSent)
            {
                bufferedEvents.Dequeue();
                advanced = true;
            }
            if (!bufferedEvents.Any(pending => pending.Seq <= lastSentSeq)) unackedSince = null;
            else if (advanced) unackedSince = DateTimeOffset.UtcNow;
        }
    }

    // Replay only happens after identify, so an open socket that stopped acknowledging must be dropped.
    private async Task WatchAcknowledgementsAsync(ClientWebSocket activeSocket, CancellationToken cancellationToken)
    {
        var interval = ackTimeout / 4;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await Task.Delay(interval, cancellationToken);
                DateTimeOffset? since;
                lock (eventGate) since = unackedSince;
                if (since is null || DateTimeOffset.UtcNow - since < ackTimeout) continue;
                log("Takaro CS2 events were not acknowledged in time; reconnecting.");
                activeSocket.Abort();
                return;
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    private void LogFrameError(string type, string message)
    {
        using var doc = JsonDocument.Parse(message);
        if (HasError(doc.RootElement)) log($"Takaro CS2 {type}: {Redact(ErrorText(doc.RootElement))}");
    }

    private async Task FlushEventsAsync(CancellationToken cancellationToken)
    {
        var activeSocket = socket;
        if (!identified || activeSocket is null || activeSocket.State != WebSocketState.Open) return;
        await eventLock.WaitAsync(cancellationToken);
        try
        {
            while (identified && activeSocket.State == WebSocketState.Open)
            {
                (long Seq, string Type, string Frame) next;
                long generation;
                lock (eventGate)
                {
                    generation = connectionGeneration;
                    next = bufferedEvents.FirstOrDefault(pending => pending.Seq > lastSentSeq);
                }
                if (next.Frame is null || !await SendAsync(activeSocket, next.Frame, cancellationToken)) break;
                lock (eventGate)
                {
                    if (generation != connectionGeneration) break;
                    lastSentSeq = next.Seq;
                    highestSentSeq = Math.Max(highestSentSeq, next.Seq);
                    unackedSince ??= DateTimeOffset.UtcNow;
                }
                log($"Takaro CS2 event sent: {next.Type}.");
            }
        }
        finally { eventLock.Release(); }
    }

    private void LogTakaroError(string message)
    {
        try
        {
            using var doc = JsonDocument.Parse(message);
            var payload = doc.RootElement.TryGetProperty("payload", out var value) ? value : doc.RootElement;
            var code = payload.ValueKind == JsonValueKind.Object && payload.TryGetProperty("code", out var codeValue)
                ? codeValue.ToString() : "unknown";
            var detail = payload.ValueKind == JsonValueKind.Object && payload.TryGetProperty("message", out var messageValue)
                ? messageValue.ToString() : "No message supplied.";
            log($"Takaro CS2 WebSocket error: code={code}, message={Redact(detail)}");
        }
        catch (JsonException)
        {
            log("Takaro CS2 WebSocket error frame was not JSON.");
        }
    }
}
