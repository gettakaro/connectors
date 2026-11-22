using System.Diagnostics;

namespace Takaro.Valheim.Core;

/// <summary>A connection owns one transient snapshot until Takaro acknowledges receiving it.</summary>
public sealed class PositionSnapshotStream
{
    public const int IntervalSeconds = 3;
    public static readonly TimeSpan AckTimeout = TimeSpan.FromSeconds(30);
    private readonly object gate = new();
    private readonly Func<long> ticks;
    private long nextSeq;
    private long pendingSeq;
    private long pendingAt;
    private long nextAt;

    public PositionSnapshotStream(Func<long>? ticks = null)
    {
        var elapsed = Stopwatch.StartNew();
        this.ticks = ticks ?? (() => elapsed.Elapsed.Ticks);
    }

    public string StreamId { get; } = Guid.NewGuid().ToString("N");

    public bool TryReserve(out long seq)
    {
        lock (gate)
        {
            seq = 0;
            if (pendingSeq != 0 || ticks() < nextAt) return false;
            seq = pendingSeq = ++nextSeq;
            pendingAt = ticks();
            return true;
        }
    }

    public void Cancel(long seq)
    {
        lock (gate)
            if (pendingSeq == seq) pendingSeq = 0;
    }

    public bool Acknowledge(string streamId, long upTo)
    {
        lock (gate)
        {
            if (streamId != StreamId || pendingSeq == 0 || upTo != pendingSeq) return false;
            pendingSeq = 0;
            nextAt = ticks() + TimeSpan.FromSeconds(IntervalSeconds).Ticks;
            return true;
        }
    }

    public bool HasTimedOut
    {
        get
        {
            lock (gate) return pendingSeq != 0 && ticks() - pendingAt >= AckTimeout.Ticks;
        }
    }
}
