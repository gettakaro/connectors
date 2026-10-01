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
    /// </summary>
    public class OutboundLedger
    {
        public class Entry
        {
            public string Json;
            public bool Replayable;
            public bool IsPing;
        }

        private readonly int _maxPending;
        private readonly int _maxInFlight;
        private readonly LinkedList<Entry> _pending = new LinkedList<Entry>();
        private readonly LinkedList<KeyValuePair<long, Entry>> _inFlight =
            new LinkedList<KeyValuePair<long, Entry>>();
        private readonly Queue<long> _pingMarks = new Queue<long>();
        private long _writeSequence;

        public OutboundLedger(int maxPending, int maxInFlight)
        {
            _maxPending = maxPending;
            _maxInFlight = maxInFlight;
        }

        /// <summary>Socket generation the in-flight entries were written on.</summary>
        public long Generation { get; private set; }

        public int PendingCount => _pending.Count;

        public int InFlightCount => _inFlight.Count;

        /// <summary>Appends a frame; returns how many oldest frames were dropped to stay under the cap.</summary>
        public int Enqueue(string json, bool replayable, bool isPing)
        {
            if (string.IsNullOrEmpty(json))
                return 0;

            _pending.AddLast(new Entry { Json = json, Replayable = replayable, IsPing = isPing });
            return TrimPending();
        }

        public Entry PeekHead()
        {
            return _pending.First?.Value;
        }

        /// <summary>The head frame was written to the socket.</summary>
        public void MarkHeadWritten()
        {
            Entry head = _pending.First?.Value;
            if (head == null)
                return;
            _pending.RemoveFirst();

            if (head.Replayable)
            {
                _inFlight.AddLast(new KeyValuePair<long, Entry>(++_writeSequence, head));
                // A link that never answers pings must not grow without bound;
                // the oldest in-flight events are the likeliest to have arrived.
                while (_inFlight.Count > _maxInFlight)
                    _inFlight.RemoveFirst();
            }

            if (head.IsPing)
                _pingMarks.Enqueue(_writeSequence);
        }

        /// <summary>The head frame cannot be sent at all; give up on it.</summary>
        public void DropHead()
        {
            if (_pending.First != null)
                _pending.RemoveFirst();
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
            int confirmed = 0;
            while (_inFlight.First != null && _inFlight.First.Value.Key <= mark)
            {
                _inFlight.RemoveFirst();
                confirmed++;
            }
            return confirmed;
        }

        /// <summary>
        /// The socket the in-flight events were written on is gone: put them back
        /// at the front of the backlog, oldest first, and start a new generation.
        /// Returns how many events were requeued.
        /// </summary>
        public int RequeueInFlight(long newGeneration)
        {
            int requeued = _inFlight.Count;
            for (LinkedListNode<KeyValuePair<long, Entry>> node = _inFlight.Last; node != null; node = node.Previous)
                _pending.AddFirst(node.Value.Value);

            _inFlight.Clear();
            _pingMarks.Clear();
            Generation = newGeneration;
            TrimPending();
            return requeued;
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
