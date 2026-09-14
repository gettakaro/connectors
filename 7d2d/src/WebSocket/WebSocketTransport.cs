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
    /// </summary>
    public class WebSocketTransport
    {
        private static WebSocketTransport _instance;
        private static readonly object _lock = new object();

        private WebSocketSharp.WebSocket _webSocket;
        private Timer _heartbeatTimer;
        private Timer _reconnectTimer;
        private volatile bool _isConnected;
        private volatile bool _shuttingDown;
        private int _reconnectAttempts;
        private const int MAX_RECONNECT_INTERVAL_SECONDS = 300;

        private BlockingCollection<string> _outbound;
        private Thread _senderThread;

        // Messages that could not be written yet (socket down / reconnecting).
        // Previously these were logged and discarded, which silently lost the
        // state-mirror seed batch whenever the first connect flapped.
        private readonly Queue<string> _pending = new Queue<string>();
        private const int MAX_PENDING_MESSAGES = 1000;
        private const int SENDER_POLL_MILLISECONDS = 1000;
        private bool _pendingOverflowLogged;

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

        private void FlushPending()
        {
            while (_pending.Count > 0)
            {
                WebSocketSharp.WebSocket socket = _webSocket;
                if (socket == null || !_isConnected)
                    return; // Keep the backlog; retry on the next poll.

                string json = _pending.Peek();
                try
                {
                    socket.Send(json);
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error($"Error sending WebSocket message: {ex.Message}");
                    Log.Exception(ex);
                    // Drop this message so one poison payload cannot wedge the queue.
                    _pending.Dequeue();
                    continue;
                }

                _pending.Dequeue();
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

                socket.OnOpen += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    _reconnectAttempts = 0;
                    LogService.Instance.Info("WebSocket connection established");

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

                    // Identify must be the first frame on the wire, ahead of any
                    // buffered backlog, so it is written directly instead of
                    // going through the outbound queue.
                    try
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
                    catch (Exception ex)
                    {
                        LogService.Instance.Error($"Failed to identify: {ex.Message}");
                        return;
                    }

                    _isConnected = true;
                    StartHeartbeat();
                };

                socket.OnMessage += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    // Ping/pong and binary control traffic must never reach the
                    // JSON request router.
                    if (!e.IsText)
                        return;

                    RequestRouter.Route(e.Data);
                };

                socket.OnError += (sender, e) =>
                {
                    LogService.Instance.Error($"WebSocket error: {e.Message}");
                    if (e.Exception != null)
                    {
                        LogService.Instance.Error(
                            $"WebSocket error detail: {e.Exception.GetType().FullName}: "
                                + $"{e.Exception.Message}"
                        );
                        Log.Exception(e.Exception);
                    }
                };

                socket.OnClose += (sender, e) =>
                {
                    if (!ReferenceEquals(socket, _webSocket))
                        return;

                    _isConnected = false;
                    LogService.Instance.Info(
                        $"WebSocket connection closed: {e.Code} - {e.Reason}"
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

        private void StartHeartbeat()
        {
            StopHeartbeatTimer();

            _heartbeatTimer = new Timer(
                state =>
                {
                    if (_isConnected)
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
            _reconnectAttempts++;

            int baseIntervalSeconds = ConfigManager.Instance.ReconnectIntervalSeconds;
            int backoffMultiplier = (int)
                Math.Min(
                    Math.Pow(2, Math.Max(0, _reconnectAttempts - 1)),
                    MAX_RECONNECT_INTERVAL_SECONDS
                );
            int intervalSeconds = Math.Min(
                baseIntervalSeconds * backoffMultiplier,
                MAX_RECONNECT_INTERVAL_SECONDS
            );
            var interval = TimeSpan.FromSeconds(intervalSeconds);

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
                }
            }
        }
    }
}
