using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading;
using Takaro.Services;

namespace Takaro.WebSocket
{
    public enum InboundKind
    {
        Ignored,
        Welcome,
        IdentifyAccepted,
        IdentifyRejected,
        Pong,
        EventAck,
        EventError,
        Request,
    }

    public sealed class InboundResult
    {
        public InboundKind Kind;

        /// <summary>Set for IdentifyAccepted and IdentifyRejected.</summary>
        public IdentifyReply Reply;
    }

    /// <summary>
    /// The connection state machine behind WebSocketTransport, free of sockets,
    /// timers and logging so it can be driven with an injected clock: the
    /// identify handshake (deadline, rejection, negotiated protocol version),
    /// classification of inbound frames, acknowledgement routing into the
    /// OutboundLedger and the replay of unconfirmed events on a new socket.
    /// Receive and sender threads share it; the flags are volatile and the
    /// unconfirmed -> confirmed transition is locked.
    /// </summary>
    public sealed class TransportSession
    {
        public const string WelcomeMessageType = "connected";
        public const string PongMessageType = "pong";

        private readonly OutboundLedger _ledger;
        private readonly Func<long> _utcTicks;
        private readonly object _confirmLock = new object();

        private volatile bool _isConnected;
        private volatile bool _isConfirmed;
        private volatile bool _identifyRejected;
        private volatile bool _deadSocketClosing;
        private volatile int _protocolVersion = -1;
        private long _generation;
        private long _openedAtTicks;
        private long _lastInboundTicks;

        private readonly ConcurrentQueue<long> _pongs = new ConcurrentQueue<long>();
        private readonly ConcurrentQueue<KeyValuePair<string, long>> _eventAcks =
            new ConcurrentQueue<KeyValuePair<string, long>>();

        public TransportSession(OutboundLedger ledger, Func<long> utcTicks)
        {
            _ledger = ledger;
            _utcTicks = utcTicks;
        }

        /// <summary>True once identify has been written on the current socket.</summary>
        public bool IsConnected => _isConnected;

        /// <summary>True once Takaro has accepted identify on the current socket.</summary>
        public bool IsConfirmed => _isConfirmed;

        public bool IdentifyRejected => _identifyRejected;

        public bool DeadSocketClosing
        {
            get => _deadSocketClosing;
            set => _deadSocketClosing = value;
        }

        /// <summary>The version Takaro selected for this socket; -1 until identify is accepted.</summary>
        public int ProtocolVersion => _protocolVersion;

        public long Generation => Interlocked.Read(ref _generation);

        /// <summary>Starts a new socket generation and returns it.</summary>
        public long BeginSocket()
        {
            _identifyRejected = false;
            _deadSocketClosing = false;
            _protocolVersion = -1;
            return Interlocked.Increment(ref _generation);
        }

        public void MarkOpened()
        {
            Interlocked.Exchange(ref _openedAtTicks, _utcTicks());
            Interlocked.Exchange(ref _lastInboundTicks, _utcTicks());
        }

        public void MarkIdentifySent()
        {
            _isConnected = true;
        }

        /// <summary>Records the inbound frame that proves the link carries traffic.</summary>
        public void MarkInbound()
        {
            Interlocked.Exchange(ref _lastInboundTicks, _utcTicks());
        }

        /// <summary>Marks the socket gone; returns its uptime in seconds, or -1 if it never opened.</summary>
        public int MarkClosed(out bool wasConfirmed)
        {
            long openedAt = Interlocked.Exchange(ref _openedAtTicks, 0);
            Interlocked.Exchange(ref _lastInboundTicks, 0);
            wasConfirmed = _isConfirmed;
            _isConnected = false;
            _isConfirmed = false;
            return openedAt == 0
                ? -1
                : (int)TimeSpan.FromTicks(_utcTicks() - openedAt).TotalSeconds;
        }

        /// <summary>The socket was detached by the transport itself.</summary>
        public void MarkDetached()
        {
            _isConnected = false;
            _isConfirmed = false;
            Interlocked.Exchange(ref _openedAtTicks, 0);
            Interlocked.Exchange(ref _lastInboundTicks, 0);
        }

        /// <summary>
        /// The unconfirmed -> confirmed transition; true only for the call that makes it.
        /// </summary>
        public bool TryConfirm()
        {
            lock (_confirmLock)
            {
                if (_isConfirmed || !_isConnected)
                    return false;
                _isConfirmed = true;
                return true;
            }
        }

        /// <summary>
        /// Classifies one inbound frame and applies its effect on the session. Nothing
        /// but the identify answer counts before identify is accepted, because the
        /// frame formats depend on the version it negotiates.
        /// </summary>
        public InboundResult HandleInbound(long generation, bool isText, string data)
        {
            MarkInbound();

            if (!isText)
                return new InboundResult { Kind = InboundKind.Ignored };

            string messageType = PeekMessageType(data);
            if (messageType == WelcomeMessageType)
                return new InboundResult { Kind = InboundKind.Welcome };

            if (messageType == WebSocketMessage.MessageTypes.IdentifyResponse)
                return HandleIdentifyResponse(data);

            if (!_isConfirmed)
                return new InboundResult { Kind = InboundKind.Ignored };

            if (messageType == PongMessageType)
            {
                _pongs.Enqueue(generation);
                return new InboundResult { Kind = InboundKind.Pong };
            }

            if (_protocolVersion == 1)
            {
                if (messageType == WebSocketMessage.MessageTypes.EventAck)
                {
                    if (
                        ProtocolHandshake.TryParseEventAck(data, out string streamId, out long upTo)
                    )
                        _eventAcks.Enqueue(new KeyValuePair<string, long>(streamId, upTo));
                    return new InboundResult { Kind = InboundKind.EventAck };
                }

                if (messageType == WebSocketMessage.MessageTypes.EventError)
                    return new InboundResult { Kind = InboundKind.EventError };
            }

            return new InboundResult { Kind = InboundKind.Request };
        }

        private InboundResult HandleIdentifyResponse(string json)
        {
            if (_isConfirmed || _identifyRejected)
                return new InboundResult { Kind = InboundKind.Ignored };

            IdentifyReply reply = ProtocolHandshake.ParseIdentifyResponse(json);
            if (!reply.Accepted)
            {
                _identifyRejected = true;
                return new InboundResult { Kind = InboundKind.IdentifyRejected, Reply = reply };
            }

            _protocolVersion = reply.ProtocolVersion;
            return new InboundResult { Kind = InboundKind.IdentifyAccepted, Reply = reply };
        }

        /// <summary>
        /// True when identify was written but never answered within the deadline.
        /// </summary>
        public bool IdentifyOverdue(int deadlineSeconds)
        {
            if (_isConfirmed || !_isConnected || _identifyRejected || _deadSocketClosing)
                return false;

            long openedAt = Interlocked.Read(ref _openedAtTicks);
            if (openedAt == 0)
                return false;

            return TimeSpan.FromTicks(_utcTicks() - openedAt).TotalSeconds >= deadlineSeconds;
        }

        /// <summary>Seconds since the current socket opened, or -1 while none is up.</summary>
        public int SecondsSinceOpened()
        {
            long openedAt = Interlocked.Read(ref _openedAtTicks);
            return openedAt == 0
                ? -1
                : (int)TimeSpan.FromTicks(_utcTicks() - openedAt).TotalSeconds;
        }

        /// <summary>Seconds since the last inbound frame, or -1 while no socket is up.</summary>
        public int SecondsSinceInbound()
        {
            long last = Interlocked.Read(ref _lastInboundTicks);
            return last == 0 ? -1 : (int)TimeSpan.FromTicks(_utcTicks() - last).TotalSeconds;
        }

        public bool IsInboundStale(int timeoutSeconds)
        {
            int seconds = SecondsSinceInbound();
            return seconds >= 0 && seconds > timeoutSeconds;
        }

        /// <summary>
        /// Applies queued pongs (confirming v0 events written before their ping, but
        /// only from the current socket) and eventAck frames (proof of receipt on
        /// whichever socket they arrive).
        /// </summary>
        public void ApplyAcknowledgements()
        {
            while (_pongs.TryDequeue(out long generation))
            {
                if (generation == _ledger.Generation)
                    _ledger.AcknowledgePong();
            }

            while (_eventAcks.TryDequeue(out KeyValuePair<string, long> ack))
                _ledger.AcknowledgeUpTo(ack.Key, ack.Value);
        }

        /// <summary>
        /// Events written on a previous socket were never confirmed; queues them again
        /// ahead of newer traffic. Returns how many were requeued.
        /// </summary>
        public int ReplayUnconfirmedOnNewSocket()
        {
            long generation = Generation;
            if (generation == _ledger.Generation)
                return 0;
            return _ledger.RequeueInFlight(generation, _protocolVersion == 0);
        }

        /// <summary>The frame to write for a ledger entry on the negotiated version.</summary>
        public string WireFrame(OutboundLedger.Entry entry)
        {
            return _protocolVersion == 1 && entry.Replayable
                ? ProtocolHandshake.WithStreamPosition(entry.Json, _ledger.StreamId, entry.Seq)
                : entry.Json;
        }

        /// <summary>
        /// Reads just the "type" field by scanning rather than deserialising: this runs
        /// on websocket-sharp's receive thread on the Mono runtime shipped with 7D2D,
        /// and the cheapest possible path is the safest one. Null when unreadable.
        /// </summary>
        public static string PeekMessageType(string json)
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

        public static string RejectionAdvice(string code)
        {
            switch (code)
            {
                case ProtocolErrorCodes.Unsupported:
                    return "Takaro refused this connector for a pending Connector Migration; "
                        + "update the mod or cancel the migration in Takaro";
                case ProtocolErrorCodes.NoSharedProtocolVersion:
                    return "this mod and Takaro share no protocol version; update the mod";
                default:
                    return "Check RegistrationToken and IdentityToken in Takaro/Config.xml";
            }
        }
    }
}
