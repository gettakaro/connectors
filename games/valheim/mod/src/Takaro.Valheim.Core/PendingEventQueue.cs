namespace Takaro.Valheim.Core;

/// <summary>
/// Outgoing game events with delivery confirmation. Takaro does not acknowledge game events,
/// but it answers a client ping with a pong, in order, on the same connection. A frame therefore
/// stays queued after it was written until the pong for a ping sent after it arrives; only then
/// is it known to have reached Takaro. Frames written to a connection that died before that
/// pong are sent again after the next identify. When full, the oldest frame is dropped.
/// Protocol version 1 numbers every frame in one stream per process (<see cref="StreamSeq"/>
/// is assigned on enqueue and kept across reconnects) and confirms with eventAck instead.
/// </summary>
public sealed class PendingEventQueue
{
    private readonly LinkedList<Entry> entries = new();
    private readonly Queue<long> checkpoints = new();
    private readonly object syncRoot = new();
    private long nextSequence = 1;
    private long nextStreamSeq = 1;

    public PendingEventQueue(int capacity)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity), "Capacity must be positive.");
        }

        Capacity = capacity;
    }

    public int Capacity { get; }

    public long Dropped { get; private set; }

    public int Count
    {
        get
        {
            lock (syncRoot)
            {
                return entries.Count;
            }
        }
    }

    public int OutstandingCheckpoints
    {
        get
        {
            lock (syncRoot)
            {
                return checkpoints.Count;
            }
        }
    }

    public void Enqueue(string frame)
    {
        lock (syncRoot)
        {
            if (entries.Count == Capacity)
            {
                entries.RemoveFirst();
                Dropped++;
            }

            entries.AddLast(new Entry(frame, nextStreamSeq++));
        }
    }

    /// <summary>The oldest frame not yet written on the current connection.</summary>
    public bool TryPeekUnsent(out string frame) => TryPeekUnsent(out frame, out _);

    /// <summary>The oldest frame not yet written on the current connection, with its stream position.</summary>
    public bool TryPeekUnsent(out string frame, out long streamSeq)
    {
        lock (syncRoot)
        {
            foreach (var entry in entries)
            {
                if (entry.Sequence == 0)
                {
                    frame = entry.Frame;
                    streamSeq = entry.StreamSeq;
                    return true;
                }
            }

            frame = string.Empty;
            streamSeq = 0;
            return false;
        }
    }

    /// <summary>A version 1 eventAck: every frame at or below <paramref name="upTo"/> is handled.</summary>
    public int AcknowledgeUpTo(long upTo)
    {
        lock (syncRoot)
        {
            var acknowledged = 0;
            while (entries.First is { } first && first.Value.StreamSeq <= upTo)
            {
                entries.RemoveFirst();
                acknowledged++;
            }

            return acknowledged;
        }
    }

    /// <summary>Records that <paramref name="frame"/> was written on the current connection.</summary>
    public void MarkSent(string frame)
    {
        lock (syncRoot)
        {
            foreach (var entry in entries)
            {
                if (entry.Sequence == 0 && ReferenceEquals(entry.Frame, frame))
                {
                    entry.Sequence = nextSequence++;
                    return;
                }
            }
        }
    }

    /// <summary>Remembers that a ping was written after every frame sent so far.</summary>
    public void AddCheckpoint()
    {
        lock (syncRoot)
        {
            checkpoints.Enqueue(nextSequence - 1);
        }
    }

    /// <summary>
    /// A pong arrived: everything written before its ping reached Takaro. On protocol version 1
    /// a pong only proves the link is alive (<paramref name="releaseFrames"/> false): Takaro
    /// can still fail to queue an event it received, so only eventAck releases frames there.
    /// </summary>
    public int ConfirmOldestCheckpoint(bool releaseFrames = true)
    {
        lock (syncRoot)
        {
            if (checkpoints.Count == 0)
            {
                return 0;
            }

            var upTo = checkpoints.Dequeue();
            if (!releaseFrames)
            {
                return 0;
            }

            var confirmed = 0;
            while (entries.First is { } first && first.Value.Sequence != 0 && first.Value.Sequence <= upTo)
            {
                entries.RemoveFirst();
                confirmed++;
            }

            return confirmed;
        }
    }

    /// <summary>The connection is gone: unconfirmed frames must be written again.</summary>
    public int ResetForNewConnection()
    {
        lock (syncRoot)
        {
            checkpoints.Clear();
            var resend = 0;
            foreach (var entry in entries)
            {
                if (entry.Sequence != 0)
                {
                    entry.Sequence = 0;
                    resend++;
                }
            }

            return resend;
        }
    }

    private sealed class Entry
    {
        public Entry(string frame, long streamSeq)
        {
            Frame = frame;
            StreamSeq = streamSeq;
        }

        public string Frame { get; }

        public long StreamSeq { get; }

        public long Sequence { get; set; }
    }
}
