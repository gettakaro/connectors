using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
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
    /// Connection lifecycle (0.1.6):
    ///   open -> identify written directly (under the send lock)
    ///        -> "unconfirmed": nothing else may be written
    ///        -> confirmed by the identify acknowledgement (any inbound frame
    ///           other than Takaro's immediate "connected" welcome), or after
    ///           CONNECTION_CONFIRM_GRACE_SECONDS of uptime
    ///        -> backlog is drained and the heartbeat starts
    ///        -> the reconnect backoff is reset only once the connection has
    ///           stayed up for CONNECTION_STABLE_SECONDS
    /// 0.1.5 confirmed on the *first* inbound frame, but Takaro's connector
    /// sends `{"type":"connected"}` the instant the socket is accepted, before
    /// it has processed identify. Confirming on it released the backlog into a
    /// socket that was not identified yet; the edge answers an unidentified
    /// `gameEvent` with a bare TCP terminate, which is the 1006 "header part of
    /// a frame could not be read" bounce (F17b / F15a).
    ///
    /// 0.1.5 also had no way to notice a dead-but-open socket: the 30 s
    /// heartbeat was written and never checked, so a blackholed link stayed
    /// "open" for minutes and every event written into it was lost (F17a).
    /// 0.1.6 tracks the last inbound frame and forces a close once nothing has
    /// arrived for INBOUND_TIMEOUT_SECONDS.
    ///
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
        private long _lastInboundTicks;
        private int _reconnectAttempts;
        private const int MAX_RECONNECT_INTERVAL_SECONDS = 300;

        // How long an open-but-silent connection must survive before we treat it
        // as good. Takaro normally answers identify well inside this window.
        private const int CONNECTION_CONFIRM_GRACE_SECONDS = 10;

        // How long a connection must stay up before the reconnect backoff is
        // reset. See ResetBackoffIfStable().
        private const int CONNECTION_STABLE_SECONDS = 10;

        private const int HEARTBEAT_INTERVAL_SECONDS = 30;

        // No inbound frame for this long means the socket is dead even though
        // the TLS layer still reports it open (F17a). Two and a half heartbeat
        // intervals: Takaro pings us every 30 s and answers our own ping, so a
        // healthy link is never this quiet.
        private const int INBOUND_TIMEOUT_SECONDS = HEARTBEAT_INTERVAL_SECONDS * 5 / 2;

        // Takaro's connector sends this the moment the socket is accepted,
        // before identify has been processed. It is not an acknowledgement.
        private const string WELCOME_MESSAGE_TYPE = "connected";

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

            // A link with no inbound traffic is dead, however long it has been
            // "open": promoting it here would undo the heartbeat watchdog's
            // verdict and resume writing into the void (seen at 10:41:14 in the
            // 0.1.6 X4d run, one second after the watchdog fired).
            if (IsInboundStale())
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
                if (socket == null || !_isConnected || !_isConfirmed || IsInboundStale())
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
                    if (IsInboundStale())
                    {
                        // The link is suspect (F17a). Send failures here say
                        // nothing about the payload, so keep the backlog intact
                        // and let the heartbeat watchdog force the reconnect.
                        return;
                    }

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

                WebSocketSharp.WebSocket socket = new WebSocketSharp.WebSocket(config.WebSocketUrl);
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
                    Interlocked.Exchange(ref _lastInboundTicks, DateTime.UtcNow.Ticks);
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
                    // Nothing may escape this handler. websocket-sharp's
                    // messagec() catches a handler exception and logs
                    // ex.ToString(); on the Mono shipped with 7D2D that walk
                    // hit an assertion in metadata.c and aborted the whole
                    // server process (signal 6) during the 0.1.6 X4d run.
                    try
                    {
                        HandleInboundFrame(socket, e);
                    }
                    catch (Exception ex)
                    {
                        LogService.Instance.Info(
                            $"Error handling inbound frame: {ex.GetType().FullName}: {ex.Message}"
                        );
                    }
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
                    Interlocked.Exchange(ref _lastInboundTicks, 0);
                    int uptimeSeconds =
                        openedAt == 0
                            ? -1
                            : (int)
                                TimeSpan.FromTicks(DateTime.UtcNow.Ticks - openedAt).TotalSeconds;

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

        /// <summary>
        /// The inbound-frame path, off the lambda so it can be wrapped in a
        /// catch-all (see OnMessage).
        /// </summary>
        private void HandleInboundFrame(WebSocketSharp.WebSocket socket, MessageEventArgs e)
        {
            if (!ReferenceEquals(socket, _webSocket))
                return;

            // Any frame at all - text, binary, or a pong - proves the link is
            // still carrying traffic. This is what the heartbeat watchdog
            // checks (F17a).
            Interlocked.Exchange(ref _lastInboundTicks, DateTime.UtcNow.Ticks);

            // Ping/pong and binary control traffic must never reach the JSON
            // request router.
            if (!e.IsText)
                return;

            // Takaro sends {"type":"connected"} the instant it accepts the
            // socket, *before* it has processed identify. Treating that as the
            // acknowledgement (0.1.5) released the backlog into a socket that
            // was not identified yet, and the edge answers an unidentified
            // gameEvent by terminating the TCP connection with no close frame -
            // the F17b bounce. Wait for a frame that can only follow identify.
            string messageType = PeekMessageType(e.Data);
            if (messageType == WELCOME_MESSAGE_TYPE)
            {
                LogService.Instance.Debug(
                    "Received Takaro welcome frame; waiting for the identify "
                        + "acknowledgement before releasing the backlog"
                );
                return;
            }

            ConfirmConnection(
                string.IsNullOrEmpty(messageType)
                    ? "inbound frame"
                    : $"inbound '{messageType}' frame"
            );

            RequestRouter.Route(e.Data);
        }

        /// <summary>
        /// Reads just the "type" field of an inbound frame, by scanning rather
        /// than deserialising: this runs on websocket-sharp's receive thread on
        /// the Mono runtime shipped with 7D2D, and the cheapest possible path is
        /// the safest one. Returns null when there is no readable type.
        /// </summary>
        private static string PeekMessageType(string json)
        {
            if (string.IsNullOrEmpty(json))
                return null;

            int key = json.IndexOf("\"type\"", StringComparison.Ordinal);
            if (key < 0)
                return null;

            int colon = json.IndexOf(':', key + 6);
            if (colon < 0)
                return null;

            int open = json.IndexOf('"', colon + 1);
            if (open < 0)
                return null;

            int close = json.IndexOf('"', open + 1);
            if (close < 0)
                return null;

            return json.Substring(open + 1, close - open - 1);
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
                LogService.Instance.Warn($"Could not attach websocket-sharp logger: {ex.Message}");
            }
        }

        private void StartHeartbeat()
        {
            StopHeartbeatTimer();

            Interlocked.Exchange(ref _lastInboundTicks, DateTime.UtcNow.Ticks);

            _heartbeatTimer = new Timer(
                state =>
                {
                    if (!_isConnected || !_isConfirmed)
                        return;

                    Send(WebSocketMessage.CreateHeartbeat());
                    CheckInboundLiveness();
                },
                null,
                TimeSpan.FromSeconds(HEARTBEAT_INTERVAL_SECONDS),
                TimeSpan.FromSeconds(HEARTBEAT_INTERVAL_SECONDS)
            );
        }

        /// <summary>
        /// Seconds since the last inbound frame, or -1 while no socket is up.
        /// </summary>
        private int SecondsSinceInbound()
        {
            long last = Interlocked.Read(ref _lastInboundTicks);
            if (last == 0)
                return -1;
            return (int)TimeSpan.FromTicks(DateTime.UtcNow.Ticks - last).TotalSeconds;
        }

        private bool IsInboundStale()
        {
            int seconds = SecondsSinceInbound();
            return seconds >= 0 && seconds > INBOUND_TIMEOUT_SECONDS;
        }

        /// <summary>
        /// A blackholed link leaves the TLS socket "open" indefinitely: writes
        /// disappear into the send buffer and websocket-sharp reports no error,
        /// so every event written into it is lost (F17a). Nothing inbound for
        /// more than INBOUND_TIMEOUT_SECONDS means the socket is dead; close it
        /// so the normal reconnect path runs and events go to the backlog
        /// instead of the void.
        /// </summary>
        private void CheckInboundLiveness()
        {
            if (_shuttingDown || !_isConnected || !IsInboundStale())
                return;

            WebSocketSharp.WebSocket socket = _webSocket;
            if (socket == null)
                return;

            LogService.Instance.Info(
                $"No inbound traffic for {SecondsSinceInbound()}s - treating socket as dead; "
                    + $"closing it ({_pending.Count} message(s) buffered)"
            );

            try
            {
                // Async: a close handshake on a blackholed link would block this
                // timer thread until the TCP send buffer drains. OnClose still
                // fires, so ScheduleReconnect() runs as usual.
                socket.CloseAsync(CloseStatusCode.Away, "No inbound traffic");
            }
            catch (Exception ex)
            {
                LogService.Instance.Info(
                    $"Error closing a dead socket: {ex.GetType().FullName}: {ex.Message}"
                );
            }
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
            int backoffExponent = Math.Min(16, Math.Max(0, _reconnectAttempts - 1));
            double backoffMultiplier = Math.Pow(2, backoffExponent);
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
            Interlocked.Exchange(ref _lastInboundTicks, 0);

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
