using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Plugin;

public sealed class TakaroWebSocketRunner : IDisposable
{
    private readonly ConnectorConfig config;
    private readonly IValheimTakaroAdapter adapter;
    private readonly TakaroRequestDispatcher dispatcher;
    private readonly IMainThreadActionScheduler mainThreadActions;
    private readonly Action<string> log;
    private readonly CancellationTokenSource shutdown = new();
    private readonly SemaphoreSlim sendLock = new(1, 1);
    private readonly PlayerLifecyclePollCoordinator lifecycleCoordinator = new();
    private readonly SuppressedResponseLogLimiter suppressedResponseLogs = new(TimeSpan.FromMinutes(1));
    private static readonly TimeSpan PlayerLifecyclePollInterval = TimeSpan.FromSeconds(5);
    private readonly PendingEventQueue pendingEvents = new(capacity: 1000);
    private readonly SemaphoreSlim flushLock = new(1, 1);
    private static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan PongTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan SendTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan IdentifyTimeout = TimeSpan.FromSeconds(30);
    private DateTimeOffset connectedAt = DateTimeOffset.MaxValue;
    private static readonly string PingFrame = """{"type":"ping"}""";
    private DateTimeOffset oldestUnansweredPingAt = DateTimeOffset.MaxValue;
    private DateTimeOffset lastPingAt = DateTimeOffset.MinValue;
    private readonly object heartbeatLock = new();
    private ClientWebSocket? socket;
    private volatile bool identified;
    private Task? runLoop;

    public TakaroWebSocketRunner(
        ConnectorConfig config,
        IValheimTakaroAdapter adapter,
        Action<string>? log = null,
        IMainThreadActionScheduler? mainThreadActions = null)
    {
        this.config = config;
        this.adapter = adapter;
        this.mainThreadActions = mainThreadActions ?? InlineMainThreadActionScheduler.Instance;
        dispatcher = new TakaroRequestDispatcher(adapter, this.mainThreadActions);
        this.log = log ?? (_ => { });
    }

    public bool IsRunning => runLoop is { IsCompleted: false };

    public Task StartAsync()
    {
        runLoop ??= Task.Run(() => RunAsync(shutdown.Token));
        return Task.CompletedTask;
    }

    /// <summary>
    /// Queues a game event and delivers it once the socket is open and identified. Called from
    /// game hooks: serialisation and sending run on a pool thread, never on the game thread.
    /// </summary>
    public Task SendGameEventAsync(string eventType, object data, CancellationToken cancellationToken = default) =>
        Task.Run(async () =>
        {
            pendingEvents.Enqueue(TakaroProtocol.CreateGameEvent(eventType, data));
            await FlushPendingEventsAsync(cancellationToken);
        }, cancellationToken);

    // Writes every not-yet-written event, then a ping. Takaro answers pings in order, so the
    // pong for that ping confirms delivery of everything written before it.
    private async Task FlushPendingEventsAsync(CancellationToken cancellationToken)
    {
        await flushLock.WaitAsync(cancellationToken);
        try
        {
            var wrote = false;
            while (identified && pendingEvents.TryPeekUnsent(out var frame))
            {
                var activeSocket = socket;
                if (activeSocket is null || activeSocket.State != WebSocketState.Open)
                {
                    return;
                }

                await SendOpenAsync(activeSocket, frame, cancellationToken);
                pendingEvents.MarkSent(frame);
                wrote = true;
            }

            if (wrote)
            {
                await SendCheckpointPingAsync(cancellationToken);
            }
        }
        finally
        {
            flushLock.Release();
        }
    }

    private async Task SendCheckpointPingAsync(CancellationToken cancellationToken)
    {
        var activeSocket = socket;
        if (activeSocket is null || activeSocket.State != WebSocketState.Open)
        {
            return;
        }

        lock (heartbeatLock)
        {
            pendingEvents.AddCheckpoint();
            var now = DateTimeOffset.UtcNow;
            lastPingAt = now;
            if (oldestUnansweredPingAt == DateTimeOffset.MaxValue)
            {
                oldestUnansweredPingAt = now;
            }
        }

        await SendOpenAsync(activeSocket, PingFrame, cancellationToken);
    }

    private void OnPong()
    {
        int confirmed;
        lock (heartbeatLock)
        {
            confirmed = pendingEvents.ConfirmOldestCheckpoint();
            oldestUnansweredPingAt = pendingEvents.OutstandingCheckpoints == 0
                ? DateTimeOffset.MaxValue
                : DateTimeOffset.UtcNow;
        }

        if (confirmed > 0)
        {
            log($"Takaro Valheim confirmed delivery of {confirmed} event(s).");
        }
    }

    // Detects a dead link that still looks open (no FIN, packets silently dropped): a ping
    // goes out every 10 s and an unanswered one older than 15 s aborts the socket, so the
    // runner reconnects and re-sends unconfirmed events.
    private async Task HeartbeatAsync(ClientWebSocket activeSocket, CancellationToken cancellationToken)
    {
        if (activeSocket.State != WebSocketState.Open)
        {
            return;
        }

        if (!identified)
        {
            if (DateTimeOffset.UtcNow - connectedAt > IdentifyTimeout)
            {
                log($"Takaro Valheim got no identify answer within {IdentifyTimeout.TotalSeconds:0} s; reconnecting.");
                activeSocket.Abort();
            }

            return;
        }

        DateTimeOffset oldest;
        DateTimeOffset last;
        lock (heartbeatLock)
        {
            oldest = oldestUnansweredPingAt;
            last = lastPingAt;
        }

        var now = DateTimeOffset.UtcNow;
        if (oldest != DateTimeOffset.MaxValue && now - oldest > PongTimeout)
        {
            log($"Takaro Valheim WebSocket heartbeat lost (no pong for {(now - oldest).TotalSeconds:0} s); reconnecting and re-sending {pendingEvents.Count} unconfirmed event(s).");
            activeSocket.Abort();
            return;
        }

        if (now - last >= HeartbeatInterval)
        {
            await flushLock.WaitAsync(cancellationToken);
            try
            {
                await SendCheckpointPingAsync(cancellationToken);
            }
            finally
            {
                flushLock.Release();
            }
        }
    }

    public void Dispose()
    {
        shutdown.Cancel();
        socket?.Dispose();
        sendLock.Dispose();
        flushLock.Dispose();
        shutdown.Dispose();
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        var attempt = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                using var client = new ClientWebSocket();
                identified = false;
                lock (heartbeatLock)
                {
                    var resend = pendingEvents.ResetForNewConnection();
                    if (resend > 0)
                    {
                        log($"Takaro Valheim will re-send {resend} event(s) Takaro did not confirm on the previous connection.");
                    }

                    oldestUnansweredPingAt = DateTimeOffset.MaxValue;
                    lastPingAt = DateTimeOffset.MinValue;
                }

                socket = client;
                // A connect into a black-holed route can hang for minutes on Mono; give up after
                // 15 s and let the backoff loop try again.
                if (attempt > 0)
                {
                    log($"Takaro Valheim WebSocket reconnect attempt {attempt}.");
                }

                await WithTimeout(
                    client.ConnectAsync(new Uri(config.TakaroWsUrl), cancellationToken),
                    ConnectTimeout,
                    "connect",
                    client,
                    cancellationToken);
                connectedAt = DateTimeOffset.UtcNow;
                log("Takaro Valheim WebSocket connected.");
                await SendAsync(client, TakaroProtocol.CreateIdentify(config), cancellationToken);
                log("Takaro Valheim identify sent.");
                attempt = 0;

                using var lifecycleShutdown = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                var lifecycleLoop = Task.Run(() => PollPlayerLifecycleAsync(client, lifecycleShutdown.Token), cancellationToken);
                try
                {
                    await ReceiveLoopAsync(client, cancellationToken);
                }
                finally
                {
                    lifecycleShutdown.Cancel();
                    try
                    {
                        await lifecycleLoop;
                    }
                    catch (OperationCanceledException) when (lifecycleShutdown.IsCancellationRequested)
                    {
                    }
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                attempt++;
                var delay = TimeSpan.FromMilliseconds(Math.Min(60000, 1000 * Math.Pow(2, attempt)));
                log($"Takaro Valheim WebSocket reconnect after error: {ex.Message}");
                await Task.Delay(delay, cancellationToken);
            }
            finally
            {
                identified = false;
                socket = null;
            }
        }
    }

    private async Task PollPlayerLifecycleAsync(ClientWebSocket socket, CancellationToken cancellationToken)
    {
        while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
        {
            try
            {
                var playersResult = await RunAdapterActionAsync(
                    () => adapter.GetPlayersAsync(cancellationToken),
                    cancellationToken);
                if (!playersResult.Success
                    || playersResult.Payload is not IEnumerable<TakaroPlayer> players)
                {
                    log($"Takaro Valheim lifecycle polling skipped because the server player list is unavailable ({playersResult.ErrorCode ?? "runtime_unavailable"}); existing lifecycle state is preserved.");
                    await Task.Delay(PlayerLifecyclePollInterval, cancellationToken);
                    continue;
                }

                var onlinePlayers = players.ToArray();
                var playersWithObservedPositions = new List<string>();
                foreach (var player in onlinePlayers)
                {
                    var location = await RunAdapterActionAsync(
                        () => adapter.GetPlayerLocationAsync(player.GameId, cancellationToken),
                        cancellationToken);
                    if (location.Success)
                    {
                        playersWithObservedPositions.Add(player.GameId);
                    }
                }

                var events = lifecycleCoordinator.Update(
                    onlinePlayers,
                    playersWithObservedPositions,
                    DateTimeOffset.UtcNow);
                foreach (var evt in events)
                {
                    if (!ValheimEventAcceptancePolicy.CanEmit(
                            evt.Type,
                            ValheimEventObservationSource.ServerPlayerSnapshot))
                    {
                        continue;
                    }

                    pendingEvents.Enqueue(TakaroProtocol.CreateGameEvent(evt.Type, evt.Data));
                    log($"Takaro Valheim {evt.Type} lifecycle event queued for {evt.Player.Name} ({evt.Player.GameId}).");
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                log($"Takaro Valheim player lifecycle polling failed: {ex.Message}");
            }

            try
            {
                await FlushPendingEventsAsync(cancellationToken);
                await HeartbeatAsync(socket, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                log($"Takaro Valheim pending event flush failed: {ex.Message}");
            }

            await Task.Delay(PlayerLifecyclePollInterval, cancellationToken);
        }
    }

    private async Task ReceiveLoopAsync(ClientWebSocket socket, CancellationToken cancellationToken)
    {
        var buffer = new byte[32 * 1024];
        while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
        {
            var result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                return;
            }

            var message = Encoding.UTF8.GetString(buffer, 0, result.Count);
            if (ContainsIgnoreCase(message, "\"type\":\"ping\"")
                || ContainsIgnoreCase(message, "\"type\": \"ping\""))
            {
                await SendAsync(socket, """{"type":"pong"}""", cancellationToken);
                continue;
            }

            if (ContainsIgnoreCase(message, "\"type\":\"pong\"")
                || ContainsIgnoreCase(message, "\"type\": \"pong\""))
            {
                OnPong();
                continue;
            }

            if (ContainsIgnoreCase(message, "\"type\":\"connected\"")
                || ContainsIgnoreCase(message, "\"type\": \"connected\""))
            {
                log("Takaro Valheim WebSocket acknowledged by Takaro; sending identify.");
                await SendAsync(socket, TakaroProtocol.CreateIdentify(config), cancellationToken);
                log("Takaro Valheim identify sent.");
                continue;
            }

            if (ContainsIgnoreCase(message, "\"type\":\"identifyResponse\"")
                || ContainsIgnoreCase(message, "\"type\": \"identifyResponse\""))
            {
                if (LogIdentifyResponse(message))
                {
                    identified = true;
                    var waiting = pendingEvents.Count;
                    if (waiting > 0)
                    {
                        log($"Takaro Valheim delivering {waiting} event(s) queued before identify.");
                    }

                    _ = Task.Run(() => FlushPendingEventsAsync(cancellationToken), cancellationToken);
                }

                continue;
            }

            if (ContainsIgnoreCase(message, "\"type\":\"error\"")
                || ContainsIgnoreCase(message, "\"type\": \"error\""))
            {
                LogTakaroError(message);
                continue;
            }

            if (!ContainsIgnoreCase(message, "\"type\":\"request\"")
                && !ContainsIgnoreCase(message, "\"type\": \"request\""))
            {
                continue;
            }

            var request = TakaroProtocol.ParseRequest(message);
            log($"Takaro Valheim request received: action={request.Action}, requestId={request.RequestId}.");
            var response = await dispatcher.DispatchAsync(request, cancellationToken);
            if (!TakaroProtocol.TryCreateActionResponse(
                    request.RequestId,
                    request.Action,
                    response,
                    out var responseFrame))
            {
                if (suppressedResponseLogs.ShouldLog(request.Action, response.ErrorCode, DateTimeOffset.UtcNow))
                {
                    log($"Takaro Valheim suppressed unsupported failure response: action={request.Action}, error={response.ErrorCode ?? "action_failed"}. The Generic Connector has no compatible failure payload for this action; Takaro will expire the pending request instead of accepting fabricated state.");
                }

                continue;
            }

            await SendAsync(socket, responseFrame!, cancellationToken);
            log($"Takaro Valheim response frame written: action={request.Action}, success={response.Success}.");
        }
    }

    private Task<TakaroActionResult> RunAdapterActionAsync(
        Func<Task<TakaroActionResult>> action,
        CancellationToken cancellationToken) =>
        mainThreadActions.ScheduleAsync(
            () => action().GetAwaiter().GetResult(),
            cancellationToken);

    private async Task SendAsync(ClientWebSocket socket, string json, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(json);
        await sendLock.WaitAsync(cancellationToken);
        try
        {
            if (socket.State == WebSocketState.Open)
            {
                await WithTimeout(
                    socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cancellationToken),
                    SendTimeout,
                    "send",
                    socket,
                    cancellationToken);
            }
        }
        finally
        {
            sendLock.Release();
        }
    }

    private async Task SendOpenAsync(ClientWebSocket socket, string json, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(json);
        await sendLock.WaitAsync(cancellationToken);
        try
        {
            if (socket.State != WebSocketState.Open)
            {
                throw new WebSocketException("Takaro WebSocket closed before a queued event could be sent.");
            }

            await WithTimeout(
                socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cancellationToken),
                SendTimeout,
                "send",
                socket,
                cancellationToken);
        }
        finally
        {
            sendLock.Release();
        }
    }

    // Mono's ClientWebSocket can ignore cancellation while the route black-holes packets, so
    // connect and send are bounded by a timer; on expiry the socket is aborted and the runner
    // reconnects. The abandoned task's exception is observed so it cannot surface later.
    private static async Task WithTimeout(Task operation, TimeSpan timeout, string what, ClientWebSocket socket, CancellationToken cancellationToken)
    {
        using var timer = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var delay = Task.Delay(timeout, timer.Token);
        var finished = await Task.WhenAny(operation, delay).ConfigureAwait(false);
        if (finished == operation)
        {
            timer.Cancel();
            await operation.ConfigureAwait(false);
            return;
        }

        _ = operation.ContinueWith(t => _ = t.Exception, TaskContinuationOptions.OnlyOnFaulted);
        cancellationToken.ThrowIfCancellationRequested();
        socket.Abort();
        throw new TimeoutException($"Takaro WebSocket {what} timed out after {timeout.TotalSeconds:0} s.");
    }

    private static bool ContainsIgnoreCase(string text, string value) =>
        text.IndexOf(value, StringComparison.OrdinalIgnoreCase) >= 0;

    private bool LogIdentifyResponse(string message)
    {
        try
        {
            using var doc = JsonDocument.Parse(message);
            var payload = doc.RootElement.TryGetProperty("payload", out var payloadElement)
                ? payloadElement
                : default;

            if (payload.ValueKind == JsonValueKind.Object
                && payload.TryGetProperty("error", out var error)
                && error.ValueKind != JsonValueKind.Null
                && error.ValueKind != JsonValueKind.Undefined)
            {
                log($"Takaro Valheim identification failed: {error}");
                return false;
            }

            if (payload.ValueKind == JsonValueKind.Object
                && payload.TryGetProperty("gameServerId", out var gameServerId)
                && gameServerId.ValueKind == JsonValueKind.String)
            {
                log($"Takaro Valheim identified as gameServerId={gameServerId.GetString()}.");
                return true;
            }

            log("Takaro Valheim identifyResponse received without gameServerId.");
            return false;
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not parse identifyResponse: {ex.Message}");
            return false;
        }
    }

    private void LogTakaroError(string message)
    {
        try
        {
            using var doc = JsonDocument.Parse(message);
            var root = doc.RootElement;
            var payload = root.TryGetProperty("payload", out var payloadElement)
                ? payloadElement
                : root;

            log($"Takaro Valheim WebSocket error message received: {payload}");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim WebSocket error message received but could not parse payload: {ex.Message}");
        }
    }
}
