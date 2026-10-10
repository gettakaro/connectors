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
    /// An identifyResponse that carries "error" is a rejection, not an
    /// acknowledgement: the reason is logged, the socket is closed and the
    /// normal backoff retries it. Until then the socket is never confirmed.
    ///
    /// Game events stay in flight until a pong confirms them (OutboundLedger)
    /// and are resent on the next socket otherwise, so the window between a
    /// link going silent and the watchdog noticing no longer loses events.
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

        // Set when Takaro answers identify with an error on the current socket.
        // Such a socket must never be treated as confirmed (see HandleInboundFrame).
        private volatile bool _identifyRejected;

        // Set once the watchdog has started closing the current socket.
        private volatile bool _deadSocketClosing;

        // Incremented for every new socket. Pongs and in-flight events are only
        // meaningful for the socket generation they belong to.
        private long _generation;

        private long _openedAtTicks;
        private long _lastInboundTicks;
        private int _reconnectAttempts;

        // Kept short: a retry is one cheap TLS handshake, and with the 45 s
        // dead-link timeout a 300 s cap left the server unreachable for up to
        // five minutes after Takaro came back (2026-10-01 outage test).
        private const int MAX_RECONNECT_INTERVAL_SECONDS = 60;

        // How long an open-but-silent connection must survive before we treat it
        // as good. Takaro normally answers identify well inside this window.
        private const int CONNECTION_CONFIRM_GRACE_SECONDS = 10;

        // How long a connection must stay up before the reconnect backoff is
        // reset. See ResetBackoffIfStable().
        private const int CONNECTION_STABLE_SECONDS = 10;

        // Every ping we send is answered with a pong, and each pong confirms the
        // game events written before it (see OutboundLedger), so the interval
        // also bounds how long an event waits to be confirmed.
        private const int HEARTBEAT_INTERVAL_SECONDS = 15;

        // No inbound frame for this long means the socket is dead even though
        // the TLS layer still reports it open (F17a): three missed pongs.
        private const int INBOUND_TIMEOUT_SECONDS = HEARTBEAT_INTERVAL_SECONDS * 3;

        private const string PONG_MESSAGE_TYPE = "pong";

        // At most one acknowledgement ping per second, however busy the events.
        private const int ACK_PING_MIN_INTERVAL_MILLISECONDS = 1000;
        private long _lastAckPingTicks;
        private const string IDENTIFY_RESPONSE_TYPE = "identifyResponse";

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

        // Serialises connection attempts: the reconnect timer and a config
        // change can both start one.
        private readonly object _connectLock = new object();

        private BlockingCollection<OutboundLedger.Entry> _outbound;
        private Thread _senderThread;

        // Frames not yet written, plus game events written but not yet confirmed
        // by a pong. Only the sender thread touches it.
        private const int MAX_PENDING_MESSAGES = 500;
        private readonly OutboundLedger _ledger = new OutboundLedger(
            MAX_PENDING_MESSAGES,
            MAX_PENDING_MESSAGES
        );

        // Pongs seen by the receive thread, tagged with their socket generation,
        // for the sender thread to apply to the ledger.
        private readonly ConcurrentQueue<long> _pongs = new ConcurrentQueue<long>();
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
            LogService.Instance.Info($"Initializing WebSocket client to {config.WebSocketUrl}");

            _outbound = new BlockingCollection<OutboundLedger.Entry>();
            _senderThread = new Thread(DrainOutbound)
            {
                IsBackground = true,
                Name = "Takaro-WsSender",
            };
            _senderThread.Start();

            config.ConnectionSettingsChanged += OnConnectionSettingsChanged;
            config.StartWatching();
            lock (_connectLock)
            {
                ConnectToServer();
            }
        }

        /// <summary>
        /// A token pasted into Config.xml, or a corrected one, takes effect
        /// without a server restart: drop the current socket and connect afresh,
        /// skipping whatever backoff a rejected token had built up.
        /// </summary>
        private void OnConnectionSettingsChanged()
        {
            lock (_connectLock)
            {
                if (_shuttingDown)
                    return;
                LogService.Instance.Info("Config.xml changed; reconnecting with the new settings");
                StopTimers();
                _reconnectAttempts = 0;
                DiscardSocket();
                ConnectToServer();
            }
        }

        public void Shutdown()
        {
            lock (_connectLock)
            {
                _shuttingDown = true;
                StopTimers();
                CloseConnection();
            }
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
            // Only game events are worth resending on a new socket: a response
            // belongs to a request Takaro has already timed out.
            _outbound.Add(
                new OutboundLedger.Entry
                {
                    Json = json,
                    Replayable = message.Type == WebSocketMessage.MessageTypes.GameEvent,
                    IsPing = message.Type == WebSocketMessage.MessageTypes.Ping,
                }
            );
        }

        public void SendErrorResponse(string requestId, string errorMessage)
        {
            Send(WebSocketMessage.CreateErrorResponse(requestId, errorMessage));
        }

        private void DrainOutbound()
        {
            while (true)
            {
                OutboundLedger.Entry entry = null;
                bool took;
                try
                {
                    took = _outbound.TryTake(out entry, SENDER_POLL_MILLISECONDS);
                }
                catch (Exception)
                {
                    // Collection completed/disposed during shutdown.
                    break;
                }

                if (took)
                    Buffer(entry);
                else if (_outbound.IsCompleted)
                    break;

                // Checked every poll, not only on the heartbeat tick, so a dead
                // link is noticed within a second of the timeout.
                CheckInboundLiveness();
                ApplyPongs();
                PromoteIfGraceElapsed();
                ResetBackoffIfStable();
                FlushPending();
            }
        }

        private void Buffer(OutboundLedger.Entry entry)
        {
            if (entry == null)
                return;

            if (_ledger.Enqueue(entry.Json, entry.Replayable, entry.IsPing) > 0)
            {
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
            if (_isConfirmed || !_isConnected || _identifyRejected || _webSocket == null)
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
                    $"WebSocket connection confirmed ({reason}); releasing {_ledger.PendingCount} "
                        + "buffered outbound message(s)"
                );
                StartHeartbeat();
            }
        }

        /// <summary>
        /// Applies pongs from the receive thread: each confirms the game events
        /// written before its ping. Pongs from an older socket are ignored.
        /// </summary>
        private void ApplyPongs()
        {
            long generation;
            while (_pongs.TryDequeue(out generation))
            {
                if (generation == _ledger.Generation)
                    _ledger.AcknowledgePong();
            }
        }

        private void FlushPending()
        {
            WebSocketSharp.WebSocket socket = _webSocket;
            if (socket == null || !_isConnected || !_isConfirmed || IsInboundStale())
                return; // Keep the backlog; retry on the next poll.

            // Events written on a previous socket were never confirmed by a
            // pong; Takaro may not have them. Send them again before anything
            // newer so the order is preserved.
            long generation = Interlocked.Read(ref _generation);
            if (generation != _ledger.Generation)
            {
                int requeued = _ledger.RequeueInFlight(generation);
                if (requeued > 0)
                {
                    LogService.Instance.Info(
                        $"Resending {requeued} game event(s) that were not confirmed "
                            + "on the previous connection"
                    );
                }
            }

            while (true)
            {
                if (_ledger.PendingCount == 0)
                {
                    // Confirm what was just written instead of waiting for the
                    // next heartbeat; a duplicate after a dead link is then only
                    // possible for events written within one round trip of it.
                    if (!_ledger.NeedsAckPing || !AckPingDue())
                        return;
                    _ledger.Enqueue(
                        Newtonsoft.Json.JsonConvert.SerializeObject(
                            WebSocketMessage.CreateHeartbeat()
                        ),
                        false,
                        true
                    );
                }

                if (!_isConnected || !_isConfirmed || IsInboundStale() || !IsSendable(socket))
                    return;

                OutboundLedger.Entry head = _ledger.PeekHead();
                try
                {
                    lock (_sendLock)
                    {
                        if (!ReferenceEquals(socket, _webSocket) || !_isConnected)
                            return;
                        socket.Send(head.Json);
                    }
                }
                catch (Exception ex)
                {
                    _headFailureCount++;
                    LogService.Instance.Info(
                        $"Error sending WebSocket message (attempt {_headFailureCount}): "
                            + $"{ex.GetType().FullName}: {ex.Message}"
                    );
                    if (IsInboundStale() || !IsSendable(socket))
                    {
                        // The link is suspect (F17a) or the socket is already
                        // closing. Send failures here say nothing about the
                        // payload, so keep the backlog intact for the next
                        // connection instead of counting them toward a drop.
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
                        _ledger.DropHead();
                        _headFailureCount = 0;
                    }
                    return; // Back off to the next poll rather than hammering.
                }

                _ledger.MarkHeadWritten();
                _headFailureCount = 0;
                _pendingOverflowLogged = false;
            }
        }

        /// <summary>
        /// A socket the watchdog is closing, or one ws-sharp no longer holds
        /// Open, cannot take a write. A late inbound frame can make the link
        /// look live again during that window, so readiness is checked on the
        /// socket itself rather than inferred from inbound traffic.
        /// </summary>
        private bool IsSendable(WebSocketSharp.WebSocket socket)
        {
            return !_deadSocketClosing && socket.ReadyState == WebSocketState.Open;
        }

        private bool AckPingDue()
        {
            long now = DateTime.UtcNow.Ticks;
            if (
                now - _lastAckPingTicks
                < TimeSpan.TicksPerMillisecond * ACK_PING_MIN_INTERVAL_MILLISECONDS
            )
                return false;
            _lastAckPingTicks = now;
            return true;
        }

        private void ConnectToServer()
        {
            try
            {
                if (_shuttingDown)
                    return;

                var config = ConfigManager.Instance;
                if (!config.WebSocketEnabled)
                {
                    LogService.Instance.Info(
                        $"The Takaro connection is disabled (Enabled is false in {ConfigManager.ModConfigPath})."
                    );
                    return;
                }

                // Connecting without a token only yields a socket Takaro never
                // identifies; wait for the config watcher instead.
                if (string.IsNullOrEmpty(config.RegistrationToken))
                {
                    LogBanner(
                        LogService.Instance.Warn,
                        "RegistrationToken not set, the server is not connected to Takaro.",
                        $"Paste the registration token from Takaro into {ConfigManager.ModConfigPath}",
                        "and save it. The mod connects within a few seconds, no restart needed."
                    );
                    return;
                }

                // Drop any previous socket first: its receive thread can still
                // raise OnClose after we have moved on, which would otherwise
                // mark the *new* connection as dead.
                DiscardSocket();

                WebSocketSharp.WebSocket socket = new WebSocketSharp.WebSocket(config.WebSocketUrl);
                long generation = Interlocked.Increment(ref _generation);
                _identifyRejected = false;
                _deadSocketClosing = false;
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
                        $"WebSocket connection established ({_ledger.PendingCount} message(s) buffered)"
                    );

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
                        HandleInboundFrame(socket, generation, e);
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

                // Handled off the receive thread, under the connect lock: a
                // config change may be replacing this socket right now, and
                // a close that is already stale must not stop the new
                // connection's timers. Taking the lock on the receive thread
                // itself could deadlock against Close(), which waits for it.
                socket.OnClose += (sender, e) =>
                    ThreadPool.QueueUserWorkItem(_ => HandleClose(socket, e));

                socket.Connect();
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error connecting to WebSocket server: {ex.Message}");
                Log.Exception(ex);
                ScheduleReconnect();
            }
        }

        private void HandleClose(WebSocketSharp.WebSocket socket, CloseEventArgs e)
        {
            try
            {
                lock (_connectLock)
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
                            + $"uptime={uptimeSeconds}s, buffered={_ledger.PendingCount}, "
                            + $"unconfirmed={_ledger.InFlightCount})"
                    );

                    StopTimers();

                    if (!_shuttingDown)
                    {
                        ScheduleReconnect();
                    }
                }
            }
            catch (Exception ex)
            {
                LogService.Instance.Error(
                    $"Error handling a closed connection: {ex.GetType().FullName}: {ex.Message}"
                );
            }
        }

        /// <summary>
        /// The inbound-frame path, off the lambda so it can be wrapped in a
        /// catch-all (see OnMessage).
        /// </summary>
        private void HandleInboundFrame(
            WebSocketSharp.WebSocket socket,
            long generation,
            MessageEventArgs e
        )
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

            // Takaro rejects a bad identify (stale registration token, unknown
            // identity) with an identifyResponse carrying "error" and keeps the
            // socket open. 0.2.0 counted that as confirmation: it logged
            // "connection confirmed", wrote the backlog into the unidentified
            // socket (the 1006 bounce) and then sat "connected" while Takaro
            // showed the server offline, with the reason never logged.
            if (
                messageType == IDENTIFY_RESPONSE_TYPE
                && ProtocolDiagnostics.TryGetIdentifyRejection(e.Data, out string reason)
            )
            {
                RejectIdentify(socket, reason);
                return;
            }

            if (_identifyRejected)
                return;

            if (messageType == PONG_MESSAGE_TYPE)
            {
                _pongs.Enqueue(generation);
                return;
            }

            ConfirmConnection(
                string.IsNullOrEmpty(messageType)
                    ? "inbound frame"
                    : $"inbound '{messageType}' frame"
            );

            RequestRouter.Route(e.Data);
        }

        // Config problems stop the connection outright, so they must stand out
        // in a busy server log.
        private static void LogBanner(Action<string, string> log, params string[] lines)
        {
            const string rule =
                "*************************************************************************";
            log(rule, "");
            foreach (string line in lines)
                log("  " + line, "");
            log(rule, "");
        }

        private void RejectIdentify(WebSocketSharp.WebSocket socket, string reason)
        {
            _identifyRejected = true;
            LogBanner(
                LogService.Instance.Error,
                $"Takaro rejected identify: {reason}.",
                $"Check RegistrationToken in {ConfigManager.ModConfigPath}. Saving a corrected",
                "token reconnects within a few seconds; until then the mod retries with backoff."
            );

            try
            {
                socket.CloseAsync(CloseStatusCode.Normal, "Identify rejected");
            }
            catch (Exception ex)
            {
                LogService.Instance.Info(
                    $"Error closing a rejected socket: {ex.GetType().FullName}: {ex.Message}"
                );
            }
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
            if (_shuttingDown || !_isConnected || _deadSocketClosing || !IsInboundStale())
                return;

            WebSocketSharp.WebSocket socket = _webSocket;
            if (socket == null)
                return;

            // Runs every sender poll; close a given socket only once.
            _deadSocketClosing = true;

            LogService.Instance.Info(
                $"No inbound traffic for {SecondsSinceInbound()}s - treating socket as dead; "
                    + $"closing it ({_ledger.PendingCount} message(s) buffered)"
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

            // A config change may replace this timer while its callback waits
            // for the lock; a replaced timer must not connect a second time.
            Timer timer = null;
            timer = new Timer(
                state =>
                {
                    lock (_connectLock)
                    {
                        if (!ReferenceEquals(_reconnectTimer, timer))
                            return;
                        ConnectToServer();
                    }
                },
                null,
                Timeout.Infinite,
                Timeout.Infinite
            );
            _reconnectTimer = timer;
            timer.Change(interval, Timeout.InfiniteTimeSpan);
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
