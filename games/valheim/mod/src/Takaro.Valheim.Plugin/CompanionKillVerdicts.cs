using Takaro.Valheim.Companion.Protocol;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Read side of the optional inventory companion's kill verdicts, for the server's own kill
/// detection. A verdict is client-reported and untrusted: use it only to veto or confirm a
/// kill the server already detected for that same authenticated peer, never to create one.
/// Thread-safe; returns false/none whenever no companion bridge is running.
/// </summary>
public static class CompanionKillVerdicts
{
    private static readonly object SyncRoot = new();
    private static volatile Backend? backend;

    /// <summary>True if the peer has an active negotiated companion session (protocol 3).</summary>
    public static bool HasSession(long peerUid)
    {
        var current = backend;
        return current is not null && current.Sessions.IsActive(peerUid, current.Clock());
    }

    /// <summary>
    /// Looks for a verdict from <paramref name="peerUid"/> for a creature whose Character.m_name
    /// is <paramref name="enemyToken"/>, received within <paramref name="window"/> of
    /// <paramref name="at"/> (compare against <see cref="DateTimeOffset.UtcNow"/>), and consumes it.
    /// </summary>
    public static bool TryTake(
        long peerUid,
        string enemyToken,
        DateTimeOffset at,
        TimeSpan window,
        out bool lastHitByLocalPlayer,
        out string lastHitAttackerKind) =>
        TryTake(peerUid, null, enemyToken, at, window, out lastHitByLocalPlayer, out lastHitAttackerKind);

    /// <summary>
    /// As <see cref="TryTake(long, string, DateTimeOffset, TimeSpan, out bool, out string)"/>,
    /// preferring the verdict for exactly <paramref name="creatureZdo"/> ("userID:id") when
    /// one exists; falls back to the token match otherwise.
    /// </summary>
    public static bool TryTake(
        long peerUid,
        string? creatureZdo,
        string enemyToken,
        DateTimeOffset at,
        TimeSpan window,
        out bool lastHitByLocalPlayer,
        out string lastHitAttackerKind)
    {
        lastHitByLocalPlayer = false;
        lastHitAttackerKind = CompanionAttackerKind.None;
        var current = backend;
        if (current is null
            || !current.Verdicts.TryTake(peerUid, creatureZdo, enemyToken, at, window, out var verdict)
            || verdict is null)
        {
            return false;
        }

        lastHitByLocalPlayer = verdict.LastHitByLocalPlayer;
        lastHitAttackerKind = verdict.LastHitAttackerKind;
        return true;
    }

    /// <summary>Connects the facade to a running bridge; called by the bridge itself.</summary>
    public static void Attach(
        CompanionSessionRegistry sessions,
        CompanionKillVerdictStore verdicts,
        Func<DateTimeOffset> sessionClock)
    {
        lock (SyncRoot)
        {
            backend = new Backend(
                sessions ?? throw new ArgumentNullException(nameof(sessions)),
                verdicts ?? throw new ArgumentNullException(nameof(verdicts)),
                sessionClock ?? throw new ArgumentNullException(nameof(sessionClock)));
        }
    }

    /// <summary>Disconnects the facade, but only from the bridge that attached it.</summary>
    public static void Detach(CompanionSessionRegistry sessions)
    {
        lock (SyncRoot)
        {
            if (backend is not null && ReferenceEquals(backend.Sessions, sessions))
            {
                backend = null;
            }
        }
    }

    private sealed class Backend
    {
        public Backend(CompanionSessionRegistry sessions, CompanionKillVerdictStore verdicts, Func<DateTimeOffset> clock)
        {
            Sessions = sessions;
            Verdicts = verdicts;
            Clock = clock;
        }

        public CompanionSessionRegistry Sessions { get; }

        public CompanionKillVerdictStore Verdicts { get; }

        public Func<DateTimeOffset> Clock { get; }
    }
}
