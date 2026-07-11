namespace Takaro.Valheim.Core;

public static class CompanionSessionRestartPolicy
{
    public static bool ShouldRestart(
        CompanionSessionSnapshot? session,
        DateTimeOffset now) =>
        session is null || now >= session.ExpiresAt;
}

public sealed record QueuedCompanionEvent(
    long Generation,
    CompanionAcceptedEvent Event);

public sealed class BoundedCompanionEventQueue
{
    private readonly int capacity;
    private readonly Queue<QueuedCompanionEvent> events = new();
    private readonly object syncRoot = new();
    private long generation;

    public BoundedCompanionEventQueue(int capacity)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity), "Capacity must be positive.");
        }

        this.capacity = capacity;
    }

    public long Generation
    {
        get
        {
            lock (syncRoot)
            {
                return generation;
            }
        }
    }

    public int Count
    {
        get
        {
            lock (syncRoot)
            {
                return events.Count;
            }
        }
    }

    public bool TryEnqueue(CompanionAcceptedEvent acceptedEvent)
    {
        if (acceptedEvent is null)
        {
            return false;
        }

        lock (syncRoot)
        {
            if (events.Count >= capacity)
            {
                return false;
            }

            events.Enqueue(new QueuedCompanionEvent(generation, acceptedEvent));
            return true;
        }
    }

    public bool TryDequeue(out QueuedCompanionEvent queuedEvent)
    {
        lock (syncRoot)
        {
            if (events.Count == 0)
            {
                queuedEvent = default!;
                return false;
            }

            queuedEvent = events.Dequeue();
            return true;
        }
    }

    public void AdvanceGeneration()
    {
        lock (syncRoot)
        {
            generation = generation == long.MaxValue ? 0 : generation + 1;
            events.Clear();
        }
    }
}
