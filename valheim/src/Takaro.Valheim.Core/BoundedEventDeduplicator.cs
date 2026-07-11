using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public sealed class BoundedEventDeduplicator
{
    private readonly int capacity;
    private readonly Dictionary<EventKey, LinkedListNode<EventKey>> entries = new();
    private readonly LinkedList<EventKey> acceptanceOrder = new();
    private readonly object syncRoot = new();

    public BoundedEventDeduplicator(int capacity)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity), "Capacity must be positive.");
        }

        this.capacity = capacity;
    }

    public bool TryAccept(long peerId, string eventId)
    {
        ValidateEventId(eventId);
        var key = new EventKey(peerId, eventId);

        lock (syncRoot)
        {
            if (entries.ContainsKey(key))
            {
                return false;
            }

            if (entries.Count == capacity)
            {
                var oldest = acceptanceOrder.First!;
                acceptanceOrder.RemoveFirst();
                entries.Remove(oldest.Value);
            }

            var node = acceptanceOrder.AddLast(key);
            entries.Add(key, node);
            return true;
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            var node = acceptanceOrder.First;
            while (node is not null)
            {
                var next = node.Next;
                if (node.Value.PeerId == peerId)
                {
                    acceptanceOrder.Remove(node);
                    entries.Remove(node.Value);
                }

                node = next;
            }
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            entries.Clear();
            acceptanceOrder.Clear();
        }
    }

    private static void ValidateEventId(string eventId)
    {
        if (string.IsNullOrWhiteSpace(eventId)
            || eventId.Length > CompanionProtocol.MaximumEventIdCharacters)
        {
            throw new ArgumentException(
                $"Event ID must contain 1 to {CompanionProtocol.MaximumEventIdCharacters} characters.",
                nameof(eventId));
        }
    }

    private readonly struct EventKey : IEquatable<EventKey>
    {
        public EventKey(long peerId, string eventId)
        {
            PeerId = peerId;
            EventId = eventId;
        }

        public long PeerId { get; }

        public string EventId { get; }

        public bool Equals(EventKey other) =>
            PeerId == other.PeerId
            && string.Equals(EventId, other.EventId, StringComparison.Ordinal);

        public override bool Equals(object? obj) =>
            obj is EventKey other && Equals(other);

        public override int GetHashCode()
        {
            unchecked
            {
                return (PeerId.GetHashCode() * 397)
                    ^ StringComparer.Ordinal.GetHashCode(EventId);
            }
        }
    }
}
