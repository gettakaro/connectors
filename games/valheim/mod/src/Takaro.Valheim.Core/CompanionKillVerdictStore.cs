using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

/// <summary>One client-reported kill verdict, stamped with the server's receive time.</summary>
public sealed record CompanionKillVerdictRecord(
    long PeerId,
    string CreatureZdo,
    string Prefab,
    string EnemyToken,
    bool LastHitByLocalPlayer,
    string LastHitAttackerKind,
    DateTimeOffset ReceivedAt);

/// <summary>
/// Small in-memory store of kill verdicts sent by inventory companions. The data is
/// client-reported and untrusted: it may only veto or confirm a kill the server itself
/// detected, keyed by the authenticated peer it arrived from. Bounded per peer and in total,
/// and entries age out, so a chatty or hostile client cannot grow it.
/// </summary>
public sealed class CompanionKillVerdictStore
{
    public static readonly TimeSpan DefaultRetention = TimeSpan.FromSeconds(30);
    public const int DefaultMaximumPerPeer = 32;
    public const int DefaultMaximumPeers = 256;

    private readonly TimeSpan retention;
    private readonly int maximumPerPeer;
    private readonly int maximumPeers;
    private readonly Dictionary<long, LinkedList<CompanionKillVerdictRecord>> verdicts = new();
    private readonly object syncRoot = new();

    public CompanionKillVerdictStore()
        : this(DefaultRetention, DefaultMaximumPerPeer, DefaultMaximumPeers)
    {
    }

    public CompanionKillVerdictStore(TimeSpan retention, int maximumPerPeer, int maximumPeers)
    {
        if (retention <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(retention));
        }
        if (maximumPerPeer <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumPerPeer));
        }
        if (maximumPeers <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumPeers));
        }

        this.retention = retention;
        this.maximumPerPeer = maximumPerPeer;
        this.maximumPeers = maximumPeers;
    }

    public bool Add(long peerId, CompanionKillVerdict verdict, DateTimeOffset receivedAt)
    {
        if (verdict is null
            || !CompanionEnvelopeCodec.IsZdoId(verdict.CreatureZdo)
            || string.IsNullOrWhiteSpace(verdict.EnemyToken)
            || string.IsNullOrWhiteSpace(verdict.Prefab)
            || !CompanionAttackerKind.IsKnown(verdict.LastHitAttackerKind))
        {
            return false;
        }

        var record = new CompanionKillVerdictRecord(
            peerId,
            verdict.CreatureZdo,
            verdict.Prefab.Trim(),
            verdict.EnemyToken.Trim(),
            verdict.LastHitByLocalPlayer,
            verdict.LastHitAttackerKind,
            receivedAt);

        lock (syncRoot)
        {
            if (!verdicts.TryGetValue(peerId, out var peerVerdicts))
            {
                if (verdicts.Count >= maximumPeers)
                {
                    return false;
                }

                peerVerdicts = new LinkedList<CompanionKillVerdictRecord>();
                verdicts.Add(peerId, peerVerdicts);
            }

            Prune(peerVerdicts, receivedAt);
            // A creature dies once: a second verdict for the same ZDO replaces the first.
            for (var node = peerVerdicts.First; node is not null; node = node.Next)
            {
                if (string.Equals(node.Value.CreatureZdo, record.CreatureZdo, StringComparison.Ordinal))
                {
                    peerVerdicts.Remove(node);
                    break;
                }
            }

            peerVerdicts.AddLast(record);
            while (peerVerdicts.Count > maximumPerPeer)
            {
                peerVerdicts.RemoveFirst();
            }

            return true;
        }
    }

    /// <summary>
    /// Takes the verdict from <paramref name="peerId"/> for a creature with
    /// <paramref name="enemyToken"/> (Character.m_name) received within
    /// <paramref name="window"/> of <paramref name="at"/>, closest in time first. A match is
    /// consumed so one verdict can never decide two kills.
    /// </summary>
    public bool TryTake(
        long peerId,
        string? creatureZdo,
        string enemyToken,
        DateTimeOffset at,
        TimeSpan window,
        out CompanionKillVerdictRecord? verdict)
    {
        verdict = null;
        if (string.IsNullOrWhiteSpace(enemyToken) || window < TimeSpan.Zero)
        {
            return false;
        }

        var token = enemyToken.Trim();
        lock (syncRoot)
        {
            if (!verdicts.TryGetValue(peerId, out var peerVerdicts))
            {
                return false;
            }

            LinkedListNode<CompanionKillVerdictRecord>? best = null;
            var bestDistance = TimeSpan.MaxValue;
            var bestZdoMatch = false;
            for (var node = peerVerdicts.First; node is not null; node = node.Next)
            {
                var candidate = node.Value;
                if (!string.Equals(candidate.EnemyToken, token, StringComparison.Ordinal))
                {
                    continue;
                }

                var distance = candidate.ReceivedAt >= at ? candidate.ReceivedAt - at : at - candidate.ReceivedAt;
                if (distance > window)
                {
                    continue;
                }

                // An exact creature match beats a closer one that only shares the token.
                var zdoMatch = creatureZdo is not null
                    && string.Equals(candidate.CreatureZdo, creatureZdo, StringComparison.Ordinal);
                if (best is null
                    || (zdoMatch && !bestZdoMatch)
                    || (zdoMatch == bestZdoMatch && distance < bestDistance))
                {
                    best = node;
                    bestDistance = distance;
                    bestZdoMatch = zdoMatch;
                }
            }

            if (best is null)
            {
                return false;
            }

            verdict = best.Value;
            peerVerdicts.Remove(best);
            if (peerVerdicts.Count == 0)
            {
                verdicts.Remove(peerId);
            }

            return true;
        }
    }

    public int Count(long peerId)
    {
        lock (syncRoot)
        {
            return verdicts.TryGetValue(peerId, out var peerVerdicts) ? peerVerdicts.Count : 0;
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            verdicts.Remove(peerId);
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            verdicts.Clear();
        }
    }

    private void Prune(LinkedList<CompanionKillVerdictRecord> peerVerdicts, DateTimeOffset now)
    {
        while (peerVerdicts.First is { } first
               && now > first.Value.ReceivedAt
               && now - first.Value.ReceivedAt > retention)
        {
            peerVerdicts.RemoveFirst();
        }
    }
}
