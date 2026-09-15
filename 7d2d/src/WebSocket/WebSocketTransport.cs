using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.Threading;
using Takaro.Config;
using Takaro.Services;
using WebSocketSharp;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Owns the WebSocket connection to Takaro: connect/identify, heartbeat,
    /// exponential-backoff reconnect, and an outbound send queue drained by a
    /// dedicated sender thread so callers (including the game main thread) never
    /// block on socket I/O. Incoming messages are handed to RequestRouter.
    ///
    /// Connection lifecycle (0.1.5):
    ///   open -> identify written directly (under the send lock)
    ///        -> "unconfirmed": nothing else may be written
    ///        -> confirmed by the first inbound frame, or after
    ///           CONNECTION_CONFIRM_GRACE_SECONDS of uptime
    ///        -> backlog is drained and the heartbeat starts
    ///        -> the reconnect backoff is reset only once the connection has
    ///           stayed up for CONNECTION_STABLE_SECONDS
    /// 0.1.4 reset the backoff the moment a socket opened, so a peer that
    /// accepted and instantly dropped every connection produced an endless
    /// 30 s loop; and it wrote the buffered backlog before identify could be
    /// accepted, which is what made those connections die.
    /// </summary>
    public class WebSocketTransport
    {
        private static WebSocketTransport _instance;
        private static readonly object _lock = new object();

        private WebSocketSharp.WebSocket _webSocket;
        private Timer _heartbeatTimer;
        private Timer _reconnectTimer;

        // True once identify has been written on the current socket.
        private volatile bool _isConnected;

        // True once the current socket is proven usable (first inbound frame or
        // grace period elapsed). Only then may queued traffic be written.
        private volatile bool _isConfirmed;

        private volatile bool _shuttingDown;
        private long _openedAtTicks;
        private int _reconnectAttempts;
        private const int MAX_RECONNECT_INTERVAL_SECONDS = 300;

        // How long an open-but-silent connection must survive before we treat it
        // as good. Takaro normally answers identify well inside this window.
        private const int CONNECTION_CONFIRM_GRACE_SECONDS = 10;

        // How long a connection must stay up before the reconnect backoff is
        // reset. See ResetBackoffIfStable().
        private const int CONNECTION_STABLE_SECONDS = 10;

        // Serialises every write on the socket. websocket-sharp as shipped with
        // 7D2D does not lock its send path (the `_forSend` field is assigned in
        // the constructor and never used), so two threads writing at once would
        // interleave frames on the SslStream.
        private readonly object _sendLock = new object();

        // Guards the unconfirmed -> confirmed transition (raced by the receive
        // thread and the sender thread's grace-period check).
        private readonly object _confirmLock = new object();

        private BlockingCollection<string> _outbound;
        private Thread _senderThread;

        // Messages that could not be written yet (socket down / unconfirmed).
        private readonly Queue<string> _pending = new Queue<string>();
        private const int MAX_PENDING_MESSAGES = 500;
        private const int SENDER_POLL_MILLISECONDS = 1000;
        private const int MAX_SEND_FAILURES_PER_MESSAGE = 3;
        private bool _pendingOverflowLogged;
        private int _headFailureCount;

        public static WebSocketTransport Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new WebSocketTransport();
                }
                return _instance;
            }
        }

        public void Initialize()
        {
            var config = ConfigManager.Instance;
            if (!config.WebSocketEnabled)
            {
                LogService.Instance.Info(
                    "WebSocket client is disabled in config. Skipping initialization."
                );
                return;
            }

            LogService.Instance.Info($"Initializing WebSocket client to {config.WebSocketUrl}");

            _outbound = new BlockingCollection<string>();
            _senderThread = new Thread(DrainOutbound)
            {
                IsBackground = true,
                Name = "Takaro-WsSender",
            };
            _senderThread.Start();

            ConnectToServer();
        }

        public void Shutdown()
        {
            _shuttingDown = true;
            StopTimers();
            CloseConnection();
            _outbound?.CompleteAdding();
            _senderThread?.Join(TimeSpan.FromSeconds(5));
            _senderThread = null;
        }

        public void Send(WebSocketMessage message)
        {
            if (_outbound == null || _outbound.IsAddingCompleted)
                return;

            string json = Newtonsoft.Json.JsonConvert.SerializeObject(message);
            LogService.Instance.Debug(
                $"Queueing WebSocket message '{message.Type}' ({message.RequestId ?? "no-request-id"})"
            );
            _outbound.Add(json);
        }

        public void SendErrorResponse(string requestId, string errorMessage)
        {
            Send(WebSocketMessage.CreateErrorResponse(requestId, errorMessage));
        }

        private void DrainOutbound()
        {
            while (true)
            {
                string json = null;
                bool took;
                try
                {
                    took = _outbound.TryTake(out json, SENDER_POLL_MILLISECONDS);
                }
                catch (Exception)
                {
                    // Collection completed/disposed during shutdown.
                    break;
                }

                if (took)
                    Buffer(json);
                else if (_outbound.IsCompleted)
                    break;

                PromoteIfGraceElapsed();
                ResetBackoffIfStable();
                FlushPending();
            }
        }

        private void Buffer(string json)
        {
            if (string.IsNullOrEmpty(json))
                return;

            _pending.Enqueue(json);
            while (_pending.Count > MAX_PENDING_MESSAGES)
            {
                _pending.Dequeue();
                _headFailureCount = 0;
                if (!_pendingOverflowLogged)
                {
                    _pendingOverflowLogged = true;
                    LogService.Instance.Warn(
                        $"Outbound buffer exceeded {MAX_PENDING_MESSAGES} messages while "
                            + "disconnected - dropping oldest messages"
                    );
                }
            }
        }

        /// <summary>
        /// The reconnect backoff is reset only once a connection has *stayed up*
        /// for CONNECTION_STABLE_SECONDS. Resetting it merely because a socket
        /// opened (0.1.4) meant a peer that accepted and then instantly dropped
        /// every connection produced an endless 30 s loop with no backoff growth.
        /// </summary>
        private void ResetBackoffIfStable()
        {
            if (!_isConnected || _reconnectAttempts == 0)
                return;

            long openedAt = Interlocked.Read(ref _openedAtTicks);
            if (openedAt == 0)
                return;

            TimeSpan uptime = TimeSpan.FromTicks(DateTime.UtcNow.Ticks - openedAt);
            if (uptime.TotalSeconds < CONNECTION_STABLE_SECONDS)
                return;

            LogService.Instance.Info(
                $"WebSocket connection stable for {(int)uptime.TotalSeconds}s - "
                    + "resetting reconnect backoff"
            );
            _reconnectAttempts = 0;
        }

        /// <summary>
        /// Treat a connection that has stayed open (without any inbound traffic)
        /// for the grace period as confirmed, so a silent-but-healthy peer does
        /// not wedge the backlog forever.
        /// </summary>
        private void PromoteIfGraceElapsed()
        {
            if (_isConfirmed || !_isConnected || _webSocket == null)
                return;

            long openedAt = Interlocked.Read(ref _openedAtTicks);
            if (openedAt == 0)
                return;

            TimeSpan uptime = TimeSpan.FromTicks(DateTime.UtcNow.Ticks - openedAt);
            if (uptime.TotalSeconds >= CONNECTION_CONFIRM_GRACE_SECONDS)
                ConfirmConnection($"open for {(int)uptime.TotalSeconds}s without a close");
        }

        /// <summary>
        /// Marks the current connection as good: resets the reconnect backoff and
        /// unblocks the outbound backlog.
        /// </summary>
        private void ConfirmConnection(string reason)
        {
            lock (_confirmLock)
            {
                if (_isConfirmed || !_isConnected)
                    return;

                _isConfirmed = true;
                LogService.Instance.Info(
                    $"WebSocket connection confirmed ({reason}); releasing {_pending.Count} "
                        + "buffered outbound message(s)"
                );
                StartHeartbeat();
            }
        }

        private void FlushPending()
        {
            while (_pending.Count > 0)
            {
                WebSocketSharp.WebSocket socket = _webSocket;
                if (socket == null || !_isConnected || !_isConfirmed)
                    return; // Keep the backlog; retry on the next poll.

                string json = _pending.Peek();
                try
                {
                    lock (_sendLock)
                    {
                        if (!ReferenceEquals(socket, _webSocket) || !_isConnected)
                            return;
                        socket.Send(json);
                    }
                }
                catch (Exception ex)
                {
                    _headFailureCount++;
                    LogService.Instance.Info(
                        $"Error sending WebSocket message (attempt {_headFailureCount}): "
                            + $"{ex.GetType().FullName}: {ex.Message}"
                    );
                    if (_headFailureCount >= MAX_SEND_FAILURES_PER_MESSAGE)
                    {
                        // Drop this message so one poison payload cannot wedge
                        // the queue; earlier versions dropped it on the first
                        // failure, which silently lost events on every flap.
                        LogService.Instance.Warn(
                            "Dropping outbound message after "
                                + $"{MAX_SEND_FAILURES_PER_MESSAGE} failed sends"
                        );
                        _pending.Dequeue();
                        _headFailureCount = 0;
                    }
                    return; // Back off to the next poll rather than hammering.
                }

                _pending.Dequeue();
                _headFailureCount = 0;
                _pendingOverflowLogged = false;
            }
        }

        private void ConnectToServer()
        {
            try
            {
                var config = ConfigManager.Instance;
                if (string.IsNullOrEmpty(config.WebSocketUrl))
                {
                    LogService.Instance.Error("WebSocket URL is not set in config.");
                    return;
                }

                // Drop any previous socket first: its receive thread can still
                // raise OnClose after we have moved on, which would otherwise
                // mark the *new* connection as dead.
                DiscardSocket();

                WebSocketSharp.WebSocket socket = new WebSocketSharp.WebSocket(
                    config.WebSocketUrl
                );
                _webSocket = socket;

                // websocket-sharp swallows the real cause of a 1006: the receive
                // loop calls abort(reason, exception) which throws the exception
                // away and only reports code 1006 through OnClose, never through
                // OnError. Its own logger is the only place the exception text
                // exists, so route it into the mod log.
                AttachLibraryLogger(socket);

                socket.OnOpen += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    Interlocked.Exchange(ref _openedAtTicks, DateTime.UtcNow.Ticks);
                    LogService.Instance.Info(
                        $"WebSocket connection established ({_pending.Count} message(s) buffered)"
                    );

                    if (
                        string.IsNullOrEmpty(config.RegistrationToken)
                        || string.IsNullOrEmpty(config.IdentityToken)
                    )
                    {
                        LogService.Instance.Error(
                            "Registration token or identity token is not set in config."
                        );
                        return;
                    }

                    // Identify must be the first and, until it is accepted, the
                    // only frame on the wire.
                    try
                    {
                        lock (_sendLock)
                        {
                            socket.Send(
                                Newtonsoft.Json.JsonConvert.SerializeObject(
                                    WebSocketMessage.CreateIdentify(
                                        config.RegistrationToken,
                                        config.IdentityToken
                                    )
                                )
                            );
                        }
                    }
                    catch (Exception ex)
                    {
                        LogService.Instance.Error(
                            $"Failed to identify: {ex.GetType().FullName}: {ex.Message}"
                        );
                        return;
                    }

                    _isConnected = true;
                };

                socket.OnMessage += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    // Any inbound frame proves the far side accepted identify.
                    ConfirmConnection("first inbound frame");

                    // Ping/pong and binary control traffic must never reach the
                    // JSON request router.
                    if (!e.IsText)
                        return;

                    RequestRouter.Route(e.Data);
                };

                socket.OnError += (sender, e) =>
                {
                    LogService.Instance.Info($"WebSocket error: {e.Message}");
                    if (e.Exception != null)
                    {
                        LogService.Instance.Info(
                            $"WebSocket error detail: {Describe(e.Exception)}"
                        );
                    }
                };

                socket.OnClose += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    long openedAt = Interlocked.Exchange(ref _openedAtTicks, 0);
                    int uptimeSeconds =
                        openedAt == 0
                            ? -1
                            : (int)TimeSpan.FromTicks(DateTime.UtcNow.Ticks - openedAt).TotalSeconds;

                    bool wasConfirmed = _isConfirmed;
                    _isConnected = false;
                    _isConfirmed = false;

                    LogService.Instance.Info(
                        $"WebSocket connection closed: {e.Code} - {e.Reason} "
                            + $"(clean={e.WasClean}, confirmed={wasConfirmed}, "
                            + $"uptime={uptimeSeconds}s, buffered={_pending.Count})"
                    );

                    StopTimers();

                    if (!_shuttingDown)
                    {
                        ScheduleReconnect();
                    }
                };

                socket.Connect();
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error connecting to WebSocket server: {ex.Message}");
                Log.Exception(ex);
                ScheduleReconnect();
            }
        }

        private static string Describe(Exception ex)
        {
            string text = $"{ex.GetType().FullName}: {ex.Message}";
            for (Exception inner = ex.InnerException; inner != null; inner = inner.InnerException)
                text += $" <- {inner.GetType().FullName}: {inner.Message}";
            return text;
        }

        private static void AttachLibraryLogger(WebSocketSharp.WebSocket socket)
        {
            try
            {
                socket.Log.Level = LogLevel.Debug;
                socket.Log.Output = (data, path) =>
                {
                    try
                    {
                        string message = data.Message ?? string.Empty;
                        if (message.Length > 2000)
                            message = message.Substring(0, 2000) + " ...(truncated)";

                        if (data.Level >= LogLevel.Warn || message.Contains("Exception"))
                            LogService.Instance.Info($"[ws-sharp {data.Level}] {message}");
                        else
                            LogService.Instance.Debug($"[ws-sharp {data.Level}] {message}");
                    }
                    catch (Exception)
                    {
                        // Never let logging kill the socket thread.
                    }
                };
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn(
                    $"Could not attach websocket-sharp logger: {ex.Message}"
                );
            }
        }

        private void StartHeartbeat()
        {
            StopHeartbeatTimer();

            _heartbeatTimer = new Timer(
                state =>
                {
                    if (_isConnected && _isConfirmed)
                    {
                        Send(WebSocketMessage.CreateHeartbeat());
                    }
                },
                null,
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(30)
            );
        }

        private void StopHeartbeatTimer()
        {
            if (_heartbeatTimer != null)
            {
                _heartbeatTimer.Dispose();
                _heartbeatTimer = null;
            }
        }

        private void ScheduleReconnect()
        {
            // Counts *consecutive* failures: it is reset only in
            // ConfirmConnection(), never merely because a socket opened, so a
            // connection that dies right after the handshake still backs off.
            _reconnectAttempts++;

            int baseIntervalSeconds = ConfigManager.Instance.ReconnectIntervalSeconds;
            double backoffMultiplier = Math.Pow(2, Math.Min(16, Math.Max(0, _reconnectAttempts - 1)));
            int intervalSeconds = (int)
                Math.Min(baseIntervalSeconds * backoffMultiplier, MAX_RECONNECT_INTERVAL_SECONDS);
            var interval = TimeSpan.FromSeconds(Math.Max(1, intervalSeconds));

            LogService.Instance.Info(
                $"Scheduling reconnect attempt {_reconnectAttempts} in {interval.TotalSeconds} seconds"
            );

            if (_reconnectTimer != null)
            {
                _reconnectTimer.Dispose();
                _reconnectTimer = null;
            }

            _reconnectTimer = new Timer(
                state =>
                {
                    ConnectToServer();
                },
                null,
                interval,
                Timeout.InfiniteTimeSpan
            );
        }

        private void StopTimers()
        {
            StopHeartbeatTimer();

            if (_reconnectTimer != null)
            {
                _reconnectTimer.Dispose();
                _reconnectTimer = null;
            }
        }

        /// <summary>
        /// Detaches and closes the current socket without touching the pending
        /// outbound backlog.
        /// </summary>
        private void DiscardSocket()
        {
            WebSocketSharp.WebSocket previous = _webSocket;
            if (previous == null)
                return;

            _webSocket = null;
            _isConnected = false;
            _isConfirmed = false;
            Interlocked.Exchange(ref _openedAtTicks, 0);

            try
            {
                if (previous.ReadyState == WebSocketState.Open)
                    previous.Close(CloseStatusCode.Away, "Reconnecting");
                else
                    ((IDisposable)previous).Dispose();
            }
            catch (Exception ex)
            {
                LogService.Instance.Debug($"Error discarding previous socket: {ex.Message}");
            }
        }

        private void CloseConnection()
        {
            if (_webSocket != null && _isConnected)
            {
                try
                {
                    _webSocket.Close(CloseStatusCode.Normal, "Application shutting down");
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error($"Error closing WebSocket connection: {ex.Message}");
                }
                finally
                {
                    _webSocket = null;
                    _isConnected = false;
                    _isConfirmed = false;
                }
            }
        }
    }
}
