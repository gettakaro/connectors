using System;
using System.Collections.Generic;

namespace Takaro.Services
{
    /// <summary>
    /// Outbound bookkeeping for the WebSocket transport: the backlog of frames
    /// not yet written, and the game events already written but not yet proven
    /// received. Owned by the transport's sender thread; not thread-safe.
    ///
    /// A silently dead link (no FIN, no RST) keeps accepting writes until the
    /// inbound watchdog notices, so every event written in that window used to
    /// be lost. Takaro answers each "ping" with "pong" on the same socket, in
    /// order, so a pong proves Takaro received every frame written before its
    /// ping. Game events stay in flight until such a pong arrives; when the
    /// socket is replaced they go back to the front of the backlog and are sent
    /// again. Delivery is therefore at-least-once: if a socket dies after Takaro
    /// read an event but before the pong came back, that event is sent twice.
    ///
    /// Protocol v1 replaces the pong with an explicit cumulative acknowledgement
    /// per event stream: every event is numbered when it is first written, and
    /// Takaro's "eventAck up to N" confirms everything numbered up to N, wherever
    /// it is. Takaro drops a number it has already seen, so a resent event is
    /// harmless there. A restart makes a new stream, so numbers never repeat.
    ///
    /// Takaro drops `log` events above 100 per 5 s per server (event-rate-limited),
    /// counted when they reach it, and the backlog after an outage holds hundreds
    /// of them. A log line therefore takes a place in the log budget from the
    /// moment it is written until logWindow after Takaro confirmed it (pong or
    /// eventAck). Lines held up on the way, behind a large response for example,
    /// keep their place, so however they bunch up Takaro never receives more than
    /// logBudget of them within logWindow. The ones over budget stay parked, in
    /// order. Responses and pings never wait for them.
    /// Other game events may pass a parked log line because an event is numbered
    /// when it is first picked to be written, not when it is queued: Takaro keeps
    /// only the highest number per stream and drops anything at or below it, so
    /// numbers must rise in write order. An event that already has a number (one
    /// requeued from a dead socket) is never passed by another event.
    /// </summary>
    public class OutboundLedger
    {
        public class Entry
        {
            public string Json;
            public bool Replayable;
            public bool IsPing;

            /// <summary>A `log` game event, written within the log budget.</summary>
            public bool IsLog;

            /// <summary>Position in the event stream; 0 for frames that are not events and for events not yet picked to be written.</summary>
            public long Seq;
        }

        private readonly int _maxPending;
        private readonly int _maxInFlight;
        private readonly LinkedList<Entry> _pending = new LinkedList<Entry>();
        private readonly LinkedList<KeyValuePair<long, Entry>> _inFlight =
            new LinkedList<KeyValuePair<long, Entry>>();
        private readonly Queue<long> _pingMarks = new Queue<long>();
        private long _writeSequence;
        private long _lastPingMark;
        private long _nextSeq = 1;

        // Takaro allows 100 log events per server in each fixed 5 s bucket and
        // 600 per 30 s; 80 per 5 s stays below both.
        public const int DefaultLogBudget = 80;
        public static readonly TimeSpan DefaultLogWindow = TimeSpan.FromSeconds(5);

        private readonly int _logBudget;
        private readonly long _logWindowTicks;
        private readonly Func<long> _utcTicks;
        private readonly Queue<long> _logConfirmedAt = new Queue<long>();
        private int _logsInFlight;

        public OutboundLedger(
            int maxPending,
            int maxInFlight,
            string streamId = null,
            int logBudget = DefaultLogBudget,
            TimeSpan? logWindow = null,
            Func<long> utcTicks = null
        )
        {
            _maxPending = maxPending;
            _maxInFlight = maxInFlight;
            StreamId = streamId ?? Guid.NewGuid().ToString("N");
            _logBudget = logBudget;
            _logWindowTicks = (logWindow ?? DefaultLogWindow).Ticks;
            _utcTicks = utcTicks ?? (() => DateTime.UtcNow.Ticks);
        }

        /// <summary>Identifies the numbering of this ledger's events; new for every process.</summary>
        public string StreamId { get; }

        /// <summary>
        /// Whether a pong proves receipt of the events before its ping. True for
        /// protocol v0; on v1 only an acknowledgement does.
        /// </summary>
        public bool ConfirmsByPong { get; private set; } = true;

        /// <summary>Socket generation the in-flight entries were written on.</summary>
        public long Generation { get; private set; }

        public int PendingCount => _pending.Count;

        public int InFlightCount => _inFlight.Count;

        /// <summary>
        /// True when events were written after the last ping, so nothing on the
        /// wire will confirm them yet. The transport answers with a ping straight
        /// away: the shorter the gap between an event and the pong that confirms
        /// it, the fewer events a dying socket can force us to send twice.
        /// </summary>
        public bool NeedsAckPing =>
            ConfirmsByPong && _inFlight.Count > 0 && _lastPingMark < _writeSequence;

        /// <summary>Appends a frame; returns how many oldest frames were dropped to stay under the cap.</summary>
        public int Enqueue(string json, bool replayable, bool isPing, bool isLog = false)
        {
            if (string.IsNullOrEmpty(json))
                return 0;

            _pending.AddLast(
                new Entry
                {
                    Json = json,
                    Replayable = replayable,
                    IsPing = isPing,
                    IsLog = replayable && isLog,
                }
            );
            return TrimPending();
        }

        /// <summary>The oldest frame, numbered if it is an event, whatever the log budget.</summary>
        public Entry PeekHead()
        {
            return Number(_pending.First?.Value);
        }

        /// <summary>
        /// The frame to write next, numbered if it is an event, or null when
        /// everything left is log lines over budget (or events behind a numbered one).
        /// </summary>
        public Entry NextToSend()
        {
            bool logsOpen = LogBudgetLeft();
            bool eventsHeld = false;
            for (LinkedListNode<Entry> node = _pending.First; node != null; node = node.Next)
            {
                Entry entry = node.Value;
                if (!entry.Replayable)
                    return entry;
                if (entry.IsLog && !logsOpen)
                {
                    // Takaro would drop it as a duplicate once a higher number got through.
                    if (entry.Seq != 0)
                        eventsHeld = true;
                    continue;
                }
                if (!eventsHeld)
                    return Number(entry);
            }
            return null;
        }

        private const int UNCONFIRMED_LOG_POLL_MILLISECONDS = 100;

        /// <summary>
        /// How long until a parked log line may be written: 0 when one may go now,
        /// -1 when no log line is parked.
        /// </summary>
        public int MillisecondsUntilLogBudget()
        {
            bool parked = false;
            foreach (Entry entry in _pending)
            {
                if (entry.IsLog)
                {
                    parked = true;
                    break;
                }
            }
            if (!parked)
                return -1;
            if (LogBudgetLeft())
                return 0;
            // Confirmations arrive on the receive thread and are applied by the
            // sender loop, so a budget held by unconfirmed lines is polled.
            if (_logConfirmedAt.Count == 0)
                return UNCONFIRMED_LOG_POLL_MILLISECONDS;
            long wait = _logConfirmedAt.Peek() + _logWindowTicks - _utcTicks();
            return (int)
                Math.Max(
                    1,
                    (wait + TimeSpan.TicksPerMillisecond - 1) / TimeSpan.TicksPerMillisecond
                );
        }

        /// <summary>The head frame was written to the socket.</summary>
        public void MarkHeadWritten()
        {
            MarkWritten(PeekHead());
        }

        /// <summary>A frame returned by NextToSend or PeekHead was written to the socket.</summary>
        public void MarkWritten(Entry head)
        {
            if (head == null || !_pending.Remove(head))
                return;

            if (head.IsLog)
                _logsInFlight++;

            if (head.Replayable)
            {
                _inFlight.AddLast(new KeyValuePair<long, Entry>(++_writeSequence, head));
                // A link that never answers pings must not grow without bound;
                // the oldest in-flight events are the likeliest to have arrived.
                while (_inFlight.Count > _maxInFlight)
                {
                    Confirmed(_inFlight.First.Value.Value);
                    _inFlight.RemoveFirst();
                }
            }

            if (head.IsPing)
            {
                _pingMarks.Enqueue(_writeSequence);
                _lastPingMark = _writeSequence;
            }
        }

        /// <summary>The frame cannot be sent at all; give up on it.</summary>
        public void Drop(Entry entry)
        {
            if (entry != null)
                _pending.Remove(entry);
        }

        /// <summary>
        /// A pong arrived: everything written before the oldest unanswered ping
        /// has been received by Takaro. Returns how many events were confirmed.
        /// </summary>
        public int AcknowledgePong()
        {
            if (_pingMarks.Count == 0)
                return 0;

            long mark = _pingMarks.Dequeue();
            if (!ConfirmsByPong)
                return 0;

            int confirmed = 0;
            while (_inFlight.First != null && _inFlight.First.Value.Key <= mark)
            {
                Confirmed(_inFlight.First.Value.Value);
                _inFlight.RemoveFirst();
                confirmed++;
            }
            return confirmed;
        }

        /// <summary>
        /// Takaro acknowledged every event of <paramref name="streamId"/> up to
        /// and including <paramref name="upTo"/>. Removes them whether they are
        /// in flight or already back in the backlog for a resend. Returns how
        /// many events were confirmed.
        /// </summary>
        public int AcknowledgeUpTo(string streamId, long upTo)
        {
            if (streamId != StreamId)
                return 0;

            int confirmed = 0;
            for (LinkedListNode<KeyValuePair<long, Entry>> node = _inFlight.First; node != null; )
            {
                LinkedListNode<KeyValuePair<long, Entry>> next = node.Next;
                if (node.Value.Value.Seq <= upTo)
                {
                    Confirmed(node.Value.Value);
                    _inFlight.Remove(node);
                    confirmed++;
                }
                node = next;
            }

            for (LinkedListNode<Entry> node = _pending.First; node != null; )
            {
                LinkedListNode<Entry> next = node.Next;
                if (node.Value.Replayable && node.Value.Seq > 0 && node.Value.Seq <= upTo)
                {
                    _pending.Remove(node);
                    confirmed++;
                }
                node = next;
            }
            return confirmed;
        }

        /// <summary>
        /// The socket the in-flight events were written on is gone: put them back
        /// at the front of the backlog, oldest first, and start a new generation
        /// that confirms by pong or by acknowledgement. Returns how many events
        /// were requeued.
        /// </summary>
        public int RequeueInFlight(long newGeneration, bool confirmsByPong = true)
        {
            ConfirmsByPong = confirmsByPong;
            int requeued = _inFlight.Count;
            for (
                LinkedListNode<KeyValuePair<long, Entry>> node = _inFlight.Last;
                node != null;
                node = node.Previous
            )
                _pending.AddFirst(node.Value.Value);

            _inFlight.Clear();
            _logsInFlight = 0;
            _pingMarks.Clear();
            _lastPingMark = _writeSequence;
            Generation = newGeneration;
            TrimPending();
            return requeued;
        }

        private Entry Number(Entry entry)
        {
            if (entry != null && entry.Replayable && entry.Seq == 0)
                entry.Seq = _nextSeq++;
            return entry;
        }

        private bool LogBudgetLeft()
        {
            long now = _utcTicks();
            while (_logConfirmedAt.Count > 0 && now - _logConfirmedAt.Peek() >= _logWindowTicks)
                _logConfirmedAt.Dequeue();
            return _logsInFlight + _logConfirmedAt.Count < _logBudget;
        }

        private void Confirmed(Entry entry)
        {
            if (!entry.IsLog)
                return;
            _logsInFlight--;
            _logConfirmedAt.Enqueue(_utcTicks());
        }

        private int TrimPending()
        {
            int dropped = 0;
            while (_pending.Count > _maxPending)
            {
                _pending.RemoveFirst();
                dropped++;
            }
            return dropped;
        }
    }
}
