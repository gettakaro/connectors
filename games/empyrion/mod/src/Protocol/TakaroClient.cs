using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Protocol
{
    /// <summary>One WebSocket connection. Abstracted so tests can run against a fake Takaro.</summary>
    public interface IWsConnection : IDisposable
    {
        void Connect(Uri uri, TimeSpan timeout);
        void Send(string text);
        /// <summary>Blocks for the next text frame; returns null when the socket closed.</summary>
        string Receive();
        void Abort();
    }

    public sealed class ClientWsConnection : IWsConnection
    {
        readonly ClientWebSocket _ws = new ClientWebSocket();
        readonly byte[] _buf = new byte[1 << 16];

        public void Connect(Uri uri, TimeSpan timeout)
        {
            _ws.Options.KeepAliveInterval = TimeSpan.FromSeconds(20);
            using (var cts = new CancellationTokenSource(timeout))
                _ws.ConnectAsync(uri, cts.Token).GetAwaiter().GetResult();
        }

        public void Send(string text)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15)))
                _ws.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cts.Token).GetAwaiter().GetResult();
        }

        public string Receive()
        {
            var sb = new StringBuilder();
            while (true)
            {
                WebSocketReceiveResult r = _ws.ReceiveAsync(new ArraySegment<byte>(_buf), CancellationToken.None).GetAwaiter().GetResult();
                if (r.MessageType == WebSocketMessageType.Close) return null;
                sb.Append(Encoding.UTF8.GetString(_buf, 0, r.Count));
                if (r.EndOfMessage) return sb.ToString();
            }
        }

        public void Abort() { try { _ws.Abort(); } catch (Exception) { } }
        public void Dispose() { try { _ws.Dispose(); } catch (Exception) { } }
    }

    public sealed class ClientOptions
    {
        public string Url;
        public string IdentityToken;
        public string RegistrationToken;
        public string ServerName;
        public TimeSpan PingInterval = TimeSpan.FromSeconds(30);
        public TimeSpan InboundTimeout = TimeSpan.FromSeconds(95);
        public TimeSpan IdentifyDeadline = TimeSpan.FromSeconds(20);
        public TimeSpan MaxBackoff = TimeSpan.FromSeconds(60);
        public TimeSpan StableAfter = TimeSpan.FromSeconds(60);
        public int OutboxCapacity = 500;
        /// <summary>When set, identify offers protocol versions [0, 1] with this Capability Manifest.</summary>
        public Func<JArray> Manifest;
        /// <summary>When set, identify names the game and the connector.</summary>
        public GameProfile Profile;
    }

    public sealed class InboundRequest
    {
        public string RequestId;
        public string Action;
        public JToken RawArgs;
    }

    /// <summary>
    /// Owns the Takaro connection: connect, identify, heartbeat, reconnect with jittered backoff, and one
    /// sender thread (ClientWebSocket allows one outstanding send). Game events raised while the link is
    /// down wait in a bounded outbox and are flushed after the next identify. Carries no game symbols.
    ///
    /// Protocol v1 (when a manifest is configured and Takaro picks it): events carry this process's streamId
    /// and a seq that only grows; each stays buffered until Takaro's eventAck covers it and is resent after a
    /// reconnect (Takaro dedupes on streamId+seq). Failed requests answer as a response with error {code, message}.
    /// </summary>
    public sealed class TakaroClient
    {
        readonly Func<IWsConnection> _factory;
        readonly ClientOptions _opt;
        readonly ILog _log;
        readonly Action<InboundRequest> _onRequest;
        readonly Random _rng = new Random();

        readonly BlockingCollection<string> _sendQueue = new BlockingCollection<string>();
        readonly Queue<PendingEvent> _outbox = new Queue<PendingEvent>();
        readonly LinkedList<PendingEvent> _unacked = new LinkedList<PendingEvent>();
        readonly object _outboxLock = new object();
        readonly string _streamId = Guid.NewGuid().ToString();
        long _seq;
        volatile int _protocolVersion;

        sealed class PendingEvent
        {
            public long Seq;
            public string Type;
            public JObject Data;
        }

        volatile IWsConnection _conn;
        volatile bool _identified;
        volatile bool _stopping;
        volatile bool _rejected;
        DateTime _lastInbound = DateTime.UtcNow;
        Thread _loop, _sender, _heart;

        public string GameServerId { get; private set; }
        public bool Identified { get { return _identified; } }
        public int Reconnects { get; private set; }
        public int DroppedEvents { get; private set; }
        /// <summary>The version Takaro chose for the current connection (0 until identified).</summary>
        public int ProtocolVersion { get { return _protocolVersion; } }
        public string StreamId { get { return _streamId; } }
        public int UnackedEvents { get { lock (_outboxLock) return _unacked.Count; } }

        public TakaroClient(Func<IWsConnection> factory, ClientOptions opt, ILog log, Action<InboundRequest> onRequest)
        {
            _factory = factory; _opt = opt; _log = log; _onRequest = onRequest;
        }

        public void Start()
        {
            _loop = new Thread(RunLoop) { IsBackground = true, Name = "takaro-ws" };
            _sender = new Thread(RunSender) { IsBackground = true, Name = "takaro-send" };
            _heart = new Thread(RunHeartbeat) { IsBackground = true, Name = "takaro-heartbeat" };
            _loop.Start(); _sender.Start(); _heart.Start();
        }

        public void Stop()
        {
            _stopping = true;
            IWsConnection c = _conn;
            if (c != null) c.Abort();
            _sendQueue.CompleteAdding();
        }

        public void SendResponse(string requestId, JToken payload)
        {
            Enqueue(new JObject { ["type"] = "response", ["requestId"] = requestId, ["payload"] = payload ?? JValue.CreateNull() });
        }

        /// <summary>v0: an error frame. v1: a response carrying error {code, message}.</summary>
        public void SendError(string requestId, string message, string code = ErrorCodes.GameError)
        {
            if (_protocolVersion >= 1)
            {
                Enqueue(new JObject
                {
                    ["type"] = "response", ["requestId"] = requestId, ["payload"] = JValue.CreateNull(),
                    ["error"] = new JObject { ["code"] = code ?? ErrorCodes.Internal, ["message"] = message },
                });
                return;
            }
            Enqueue(new JObject { ["type"] = "error", ["requestId"] = requestId, ["payload"] = new JObject { ["error"] = message } });
        }

        /// <summary>Replaces the Capability Manifest on a v1 connection (a new planet got terrain). False on v0.</summary>
        public bool SendCapabilities()
        {
            if (_opt.Manifest == null || !_identified || _protocolVersion < 1) return false;
            Enqueue(new JObject { ["type"] = "capabilities", ["requestId"] = Guid.NewGuid().ToString(), ["payload"] = new JObject { ["capabilities"] = _opt.Manifest() } });
            return true;
        }

        public void SendGameEvent(string type, JObject data)
        {
            var ev = new PendingEvent { Type = type, Data = data };
            lock (_outboxLock)
            {
                if (_identified && _conn != null) { Dispatch(ev); return; }
                if (_outbox.Count >= _opt.OutboxCapacity) { _outbox.Dequeue(); DroppedEvents++; }
                _outbox.Enqueue(ev);
            }
        }

        /// <summary>Queues one event for the socket; on v1 numbers it and keeps it until acked. Caller holds _outboxLock.</summary>
        void Dispatch(PendingEvent ev)
        {
            var payload = new JObject { ["type"] = ev.Type, ["data"] = ev.Data };
            if (_protocolVersion >= 1)
            {
                if (ev.Seq == 0)
                {
                    ev.Seq = ++_seq;
                    _unacked.AddLast(ev);
                    if (_unacked.Count > _opt.OutboxCapacity) { _unacked.RemoveFirst(); DroppedEvents++; }
                }
                payload["streamId"] = _streamId;
                payload["seq"] = ev.Seq;
            }
            try { _sendQueue.Add(Serialize(new JObject { ["type"] = "gameEvent", ["payload"] = payload })); }
            catch (InvalidOperationException) { }
        }

        void Acknowledge(JToken payload)
        {
            if (!(payload is JObject p) || (string)p["streamId"] != _streamId || p["upTo"]?.Type != JTokenType.Integer) return;
            long upTo = (long)p["upTo"];
            lock (_outboxLock)
                while (_unacked.First != null && _unacked.First.Value.Seq <= upTo) _unacked.RemoveFirst();
        }

        void Enqueue(JObject msg)
        {
            if (!_identified) return; // a reply for a dead socket's request has nowhere to go
            try { _sendQueue.Add(Serialize(msg)); } catch (InvalidOperationException) { }
        }

        static string Serialize(JObject o) { return o.ToString(Formatting.None); }

        void RunSender()
        {
            foreach (string frame in _sendQueue.GetConsumingEnumerable())
            {
                IWsConnection c = _conn;
                // Before identifyResponse Takaro drops the socket on anything but identify; v1 events come back from _unacked.
                if (c == null || !_identified) continue;
                try
                {
                    c.Send(frame);
                    _log.Debug("SEND " + LogText.Clip(frame));
                }
                catch (Exception e)
                {
                    _log.Warn("send failed: " + e.GetType().Name + ": " + e.Message);
                    c.Abort();
                }
            }
        }

        void RunHeartbeat()
        {
            while (!_stopping)
            {
                Thread.Sleep(_opt.PingInterval);
                IWsConnection c = _conn;
                if (c == null || !_identified) continue;
                if (DateTime.UtcNow - _lastInbound > _opt.InboundTimeout)
                {
                    _log.Warn("no frame from Takaro for " + (int)_opt.InboundTimeout.TotalSeconds + "s, reconnecting");
                    c.Abort();
                    continue;
                }
                try { _sendQueue.Add("{\"type\":\"ping\"}"); } catch (InvalidOperationException) { }
            }
        }

        void RunLoop()
        {
            int attempt = 0;
            while (!_stopping)
            {
                DateTime connectedAt = DateTime.MinValue;
                IWsConnection c = null;
                try
                {
                    c = _factory();
                    c.Connect(new Uri(_opt.Url), TimeSpan.FromSeconds(20));
                    _lastInbound = DateTime.UtcNow;
                    _conn = c;
                    c.Send(Serialize(BuildIdentify()));
                    _log.Info("connected to " + _opt.Url + ", identify sent");
                    DateTime deadline = DateTime.UtcNow + _opt.IdentifyDeadline;
                    var watchdog = new Timer(_ => { if (!_identified && DateTime.UtcNow >= deadline) c.Abort(); }, null, _opt.IdentifyDeadline, Timeout.InfiniteTimeSpan);
                    try
                    {
                        while (!_stopping)
                        {
                            string text = c.Receive();
                            if (text == null) { _log.Warn("Takaro closed the connection"); break; }
                            _lastInbound = DateTime.UtcNow;
                            if (HandleFrame(text) && connectedAt == DateTime.MinValue) connectedAt = DateTime.UtcNow;
                            if (connectedAt != DateTime.MinValue && attempt > 0 && DateTime.UtcNow - connectedAt > _opt.StableAfter) attempt = 0;
                        }
                    }
                    finally { watchdog.Dispose(); }
                }
                catch (Exception e)
                {
                    if (!_stopping && !_rejected) _log.Warn("connection error: " + e.GetType().Name + ": " + LogText.Clip(e.Message));
                }
                finally
                {
                    _identified = false;
                    _protocolVersion = 0;
                    _rejected = false;
                    _conn = null;
                    if (c != null) { c.Abort(); c.Dispose(); }
                }
                if (_stopping) break;
                if (connectedAt != DateTime.MinValue && DateTime.UtcNow - connectedAt > _opt.StableAfter) attempt = 0;
                Reconnects++;
                double baseSec = Math.Min(_opt.MaxBackoff.TotalSeconds, Math.Pow(2, Math.Min(attempt, 6)));
                double wait = baseSec * (0.5 + _rng.NextDouble() * 0.5);
                attempt++;
                _log.Info("reconnecting in " + wait.ToString("0.0") + "s (attempt " + attempt + ")");
                Thread.Sleep(TimeSpan.FromSeconds(wait));
            }
        }

        JObject BuildIdentify()
        {
            var p = new JObject { ["identityToken"] = _opt.IdentityToken, ["name"] = _opt.ServerName };
            if (!string.IsNullOrEmpty(_opt.RegistrationToken)) p["registrationToken"] = _opt.RegistrationToken;
            _opt.Profile?.AddToIdentify(p);
            if (_opt.Manifest != null)
            {
                p["protocolVersions"] = new JArray(Capabilities.OfferedVersions);
                p["capabilities"] = _opt.Manifest();
            }
            return new JObject { ["type"] = "identify", ["payload"] = p };
        }

        /// <summary>Returns true when this frame confirmed the identify.</summary>
        bool HandleFrame(string text)
        {
            JObject msg;
            try { msg = JObject.Parse(text); }
            catch (Exception) { _log.Warn("unparseable frame: " + LogText.Clip(text)); return false; }
            string type = (string)msg["type"];
            if (type != "pong") _log.Debug("RECV " + LogText.Clip(text));
            switch (type)
            {
                case "identifyResponse":
                    // v0 puts a refusal in payload.error, v1 in a top-level error (and then closes the socket).
                    JObject payload = msg["payload"] as JObject;
                    JToken err = payload?["error"];
                    if (err == null || err.Type == JTokenType.Null) err = msg["error"];
                    if (err != null && err.Type != JTokenType.Null)
                    {
                        string why = err is JObject eo ? ((string)eo["message"] ?? eo.ToString(Formatting.None)) : err.ToString();
                        string hint = why.Contains("409")
                            ? " - Takaro already has a game server with this SERVER_NAME under a different IDENTITY_TOKEN; restore the old IDENTITY_TOKEN or pick a new SERVER_NAME"
                            : why.Contains("401") || why.Contains("403") ? " - check REGISTRATION_TOKEN" : "";
                        _log.Error("Takaro rejected identify: " + LogText.Clip(why, 200) + hint);
                        _rejected = true;
                        _conn?.Abort();
                        return false;
                    }
                    GameServerId = (string)payload?["gameServerId"];
                    JToken version = payload?["protocolVersion"];
                    _protocolVersion = _opt.Manifest != null && version != null && version.Type == JTokenType.Integer && (int)version == 1 ? 1 : 0;
                    lock (_outboxLock)
                    {
                        _identified = true;
                        FlushOutbox();
                    }
                    _log.Info("identified as game server " + GameServerId + ", protocol version " + _protocolVersion);
                    return true;
                case "request":
                    var req = new InboundRequest
                    {
                        RequestId = (string)msg["requestId"],
                        Action = (string)msg["payload"]?["action"],
                        RawArgs = msg["payload"]?["args"],
                    };
                    try { _onRequest(req); }
                    catch (Exception e) { _log.Error("request dispatch threw: " + e); SendError(req.RequestId, "internal error"); }
                    return false;
                case "error":
                    _log.Warn("Takaro reported an error: " + LogText.Clip(text));
                    return false;
                case "eventAck":
                    Acknowledge(msg["payload"]);
                    return false;
                case "eventError":
                    // Takaro refused one numbered event (validation); it is acknowledged too, so it is not resent.
                    _log.Warn("Takaro rejected an event: " + LogText.Clip(text));
                    return false;
                case "capabilitiesResponse":
                    if (msg["error"] != null && msg["error"].Type != JTokenType.Null) _log.Error("Takaro rejected the capability manifest: " + LogText.Clip(msg["error"].ToString(Formatting.None)));
                    else _log.Info("Takaro accepted the updated capability manifest");
                    return false;
                case "ping":
                    try { _sendQueue.Add("{\"type\":\"pong\"}"); } catch (InvalidOperationException) { }
                    return false;
                default:
                    return false;
            }
        }

        /// <summary>After identify: unacked v1 events first (same seq), then events raised while offline. Caller holds _outboxLock.</summary>
        void FlushOutbox()
        {
            int resend = _unacked.Count, queued = _outbox.Count;
            if (resend + queued > 0) _log.Info("flushing " + queued + " queued game events" + (resend > 0 ? ", resending " + resend + " unacknowledged" : ""));
            var again = new List<PendingEvent>(_unacked);
            if (_protocolVersion < 1) _unacked.Clear();
            foreach (PendingEvent ev in again) Dispatch(ev);
            while (_outbox.Count > 0) Dispatch(_outbox.Dequeue());
        }
    }

    public interface ILog
    {
        void Debug(string s);
        void Info(string s);
        void Warn(string s);
        void Error(string s);
    }

    public static class LogText
    {
        public static string Clip(string s, int max = 600)
        {
            if (s == null) return "";
            s = System.Text.RegularExpressions.Regex.Replace(s, "(\"(?:registrationToken|identityToken)\"\\s*:\\s*\")[^\"]*", "$1<redacted>");
            return s.Length > max ? s.Substring(0, max) + "…(" + s.Length + ")" : s;
        }
    }
}
