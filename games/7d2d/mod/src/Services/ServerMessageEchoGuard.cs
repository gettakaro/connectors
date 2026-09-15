using System;
using System.Collections.Generic;

namespace Takaro.Services
{
    /// <summary>
    /// Suppresses the native server-log line produced by a recent Takaro
    /// sendMessage action. Without this seam, a Takaro log hook that responds
    /// with a server message can consume and re-emit its own output forever.
    /// </summary>
    public sealed class ServerMessageEchoGuard
    {
        private const string GlobalServerChatPrefix =
            "Chat (from '-non-player-', entity id '-1', to 'Global'): ";
        private static readonly TimeSpan EntryLifetime = TimeSpan.FromSeconds(30);

        public const int MaxEntries = 128;
        public static readonly ServerMessageEchoGuard Instance = new ServerMessageEchoGuard();

        private readonly object _sync = new object();
        private readonly LinkedList<Entry> _entries = new LinkedList<Entry>();

        public void Record(string renderedMessage)
        {
            Record(renderedMessage, DateTimeOffset.UtcNow);
        }

        public void Record(string renderedMessage, DateTimeOffset now)
        {
            if (string.IsNullOrEmpty(renderedMessage))
                return;

            lock (_sync)
            {
                PruneExpired(now);
                _entries.AddLast(new Entry(renderedMessage, now));
                while (_entries.Count > MaxEntries)
                    _entries.RemoveFirst();
            }
        }

        public bool ShouldSuppress(string plainMessage)
        {
            return ShouldSuppress(plainMessage, DateTimeOffset.UtcNow);
        }

        public bool ShouldSuppress(string plainMessage, DateTimeOffset now)
        {
            if (
                string.IsNullOrEmpty(plainMessage)
                || !plainMessage.StartsWith(GlobalServerChatPrefix, StringComparison.Ordinal)
            )
                return false;

            lock (_sync)
            {
                PruneExpired(now);
                LinkedListNode<Entry> node = _entries.Last;
                while (node != null)
                {
                    LinkedListNode<Entry> previous = node.Previous;
                    if (
                        string.Equals(
                            plainMessage,
                            GlobalServerChatPrefix + node.Value.RenderedMessage,
                            StringComparison.Ordinal
                        )
                    )
                    {
                        _entries.Remove(node);
                        return true;
                    }
                    node = previous;
                }
            }

            return false;
        }

        private void PruneExpired(DateTimeOffset now)
        {
            while (_entries.First != null && now - _entries.First.Value.RecordedAt > EntryLifetime)
            {
                _entries.RemoveFirst();
            }
        }

        private sealed class Entry
        {
            public Entry(string renderedMessage, DateTimeOffset recordedAt)
            {
                RenderedMessage = renderedMessage;
                RecordedAt = recordedAt;
            }

            public string RenderedMessage { get; }
            public DateTimeOffset RecordedAt { get; }
        }
    }
}
