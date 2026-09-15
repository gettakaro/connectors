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
    private ClientWebSocket? socket;
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

    public Task SendGameEventAsync(string eventType, object data, CancellationToken cancellationToken = default) =>
        Task.Run(() => SendGameEventCoreAsync(eventType, data, cancellationToken), cancellationToken);

    private async Task SendGameEventCoreAsync(string eventType, object data, CancellationToken cancellationToken)
    {
        var activeSocket = socket;
        if (activeSocket is null || activeSocket.State != WebSocketState.Open)
        {
            return;
        }

        await SendAsync(activeSocket, TakaroProtocol.CreateGameEvent(eventType, data), cancellationToken);
    }

    public void Dispose()
    {
        shutdown.Cancel();
        socket?.Dispose();
        sendLock.Dispose();
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
                socket = client;
                await client.ConnectAsync(new Uri(config.TakaroWsUrl), cancellationToken);
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

                    await SendAsync(socket, TakaroProtocol.CreateGameEvent(evt.Type, evt.Data), cancellationToken);
                    log($"Takaro Valheim {evt.Type} lifecycle frame written for {evt.Player.Name} ({evt.Player.GameId}); Takaro persistence is not acknowledged by the Generic Connector transport.");
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
                LogIdentifyResponse(message);
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
                await socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cancellationToken);
            }
        }
        finally
        {
            sendLock.Release();
        }
    }

    private static bool ContainsIgnoreCase(string text, string value) =>
        text.IndexOf(value, StringComparison.OrdinalIgnoreCase) >= 0;

    private void LogIdentifyResponse(string message)
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
                return;
            }

            if (payload.ValueKind == JsonValueKind.Object
                && payload.TryGetProperty("gameServerId", out var gameServerId)
                && gameServerId.ValueKind == JsonValueKind.String)
            {
                log($"Takaro Valheim identified as gameServerId={gameServerId.GetString()}.");
                return;
            }

            log("Takaro Valheim identifyResponse received without gameServerId.");
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim could not parse identifyResponse: {ex.Message}");
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
