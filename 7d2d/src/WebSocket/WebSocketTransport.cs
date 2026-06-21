using System;
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
            LogService.Instance.Debug($"Queueing WebSocket message: {json}");
            _outbound.Add(json);
        }

        public void SendErrorResponse(string requestId, string errorMessage)
        {
            Send(WebSocketMessage.CreateErrorResponse(requestId, errorMessage));
        }

        private void DrainOutbound()
        {
            foreach (string json in _outbound.GetConsumingEnumerable())
            {
                if (_webSocket == null || !_isConnected)
                {
                    LogService.Instance.Warn("Cannot send message - WebSocket not connected");
                    continue;
                }

                try
                {
                    _webSocket.Send(json);
                }
                catch (Exception ex)
                {
                    LogService.Instance.Error($"Error sending WebSocket message: {ex.Message}");
                    Log.Exception(ex);
                }
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

                _webSocket = new WebSocketSharp.WebSocket(config.WebSocketUrl);

                _webSocket.OnOpen += (sender, e) =>
                {
                    _isConnected = true;
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

                    Send(
                        WebSocketMessage.CreateIdentify(
                            config.RegistrationToken,
                            config.IdentityToken
                        )
                    );
                    StartHeartbeat();
                };

                _webSocket.OnMessage += (sender, e) =>
                {
                    RequestRouter.Route(e.Data);
                };

                _webSocket.OnError += (sender, e) =>
                {
                    LogService.Instance.Error($"WebSocket error: {e.Message}");
                };

                _webSocket.OnClose += (sender, e) =>
                {
                    _isConnected = false;
                    LogService.Instance.Info($"WebSocket connection closed: {e.Code} - {e.Reason}");

                    StopTimers();

                    if (!_shuttingDown)
                    {
                        ScheduleReconnect();
                    }
                };

                _webSocket.Connect();
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
