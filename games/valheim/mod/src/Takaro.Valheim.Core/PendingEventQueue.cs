namespace Takaro.Valheim.Core;

/// <summary>
/// Outgoing game events with delivery confirmation. Takaro does not acknowledge game events,
/// but it answers a client ping with a pong, in order, on the same connection. A frame therefore
/// stays queued after it was written until the pong for a ping sent after it arrives; only then
/// is it known to have reached Takaro. Frames written to a connection that died before that
/// pong are sent again after the next identify. When full, the oldest frame is dropped.
/// </summary>
public sealed class PendingEventQueue
{
    private readonly LinkedList<Entry> entries = new();
    private readonly Queue<long> checkpoints = new();
    private readonly object syncRoot = new();
    private long nextSequence = 1;

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

            entries.AddLast(new Entry(frame));
        }
    }

    /// <summary>The oldest frame not yet written on the current connection.</summary>
    public bool TryPeekUnsent(out string frame)
    {
        lock (syncRoot)
        {
            foreach (var entry in entries)
            {
                if (entry.Sequence == 0)
                {
                    frame = entry.Frame;
                    return true;
                }
            }

            frame = string.Empty;
            return false;
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

    /// <summary>A pong arrived: everything written before its ping reached Takaro.</summary>
    public int ConfirmOldestCheckpoint()
    {
        lock (syncRoot)
        {
            if (checkpoints.Count == 0)
            {
                return 0;
            }

            var upTo = checkpoints.Dequeue();
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
        public Entry(string frame)
        {
            Frame = frame;
        }

        public string Frame { get; }

        public long Sequence { get; set; }
    }
}
