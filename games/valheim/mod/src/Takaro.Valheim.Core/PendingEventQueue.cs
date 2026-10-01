namespace Takaro.Valheim.Core;

/// <summary>
/// Holds outgoing game events until Takaro has accepted this connector's identify. A frame is
/// removed only after it was written to an identified socket, so events raised while the
/// connector is still identifying, reconnecting or offline are delivered later instead of being
/// written into a socket Takaro has not accepted yet. When full, the oldest event is dropped.
/// </summary>
public sealed class PendingEventQueue
{
    private readonly LinkedList<string> frames = new();
    private readonly object syncRoot = new();

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
                return frames.Count;
            }
        }
    }

    public void Enqueue(string frame)
    {
        lock (syncRoot)
        {
            if (frames.Count == Capacity)
            {
                frames.RemoveFirst();
                Dropped++;
            }

            frames.AddLast(frame);
        }
    }

    public bool TryPeek(out string frame)
    {
        lock (syncRoot)
        {
            if (frames.First is null)
            {
                frame = string.Empty;
                return false;
            }

            frame = frames.First.Value;
            return true;
        }
    }

    /// <summary>Removes the head frame if it is still the one that was just sent.</summary>
    public void Acknowledge(string frame)
    {
        lock (syncRoot)
        {
            if (frames.First is not null && ReferenceEquals(frames.First.Value, frame))
            {
                frames.RemoveFirst();
            }
        }
    }
}
