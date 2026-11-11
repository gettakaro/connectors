using System;

namespace TakaroEmpyrion.Protocol
{
    /// <summary>
    /// When to reconnect to Takaro. A connection that was identified and stayed up for
    /// <see cref="ClientOptions.StableAfter"/> starts its next attempt 0.05-0.35 s after it dropped (jittered, so a
    /// Takaro deploy does not bring every server back in the same instant). With the TLS and identify round trip it
    /// is identified again within about 2 s, inside Takaro's reachability window. Only consecutive failures back
    /// off: 1-2 s, 2-4 s, 4-8 s, ... with jitter, never past the cap (60 s by default). The streak resets once a
    /// connection has been stable again.
    /// </summary>
    public sealed class ReconnectPolicy
    {
        public const int FastMinMs = 50;
        public const int FastSpreadMs = 300;
        public const int BackoffBaseMs = 1000;

        readonly TimeSpan _stableAfter;
        readonly TimeSpan _max;
        int _failures;

        public ReconnectPolicy(TimeSpan stableAfter, TimeSpan max)
        {
            _stableAfter = stableAfter;
            _max = max;
        }

        /// <summary>Attempts since the last stable connection.</summary>
        public int Failures { get { return _failures; } }

        /// <summary>The wait after <paramref name="failures"/> attempts in a row; random01 is in [0, 1).</summary>
        public static TimeSpan Delay(int failures, double random01, TimeSpan max)
        {
            if (random01 < 0) random01 = 0;
            if (random01 >= 1) random01 = 0.999999;
            double maxMs = Math.Max(0, max.TotalMilliseconds);
            if (failures <= 0) return TimeSpan.FromMilliseconds(Math.Min(maxMs, FastMinMs + FastSpreadMs * random01));
            double ceiling = Math.Min(maxMs, BackoffBaseMs * Math.Pow(2, Math.Min(failures, 16)));
            return TimeSpan.FromMilliseconds(ceiling / 2 + ceiling / 2 * random01);
        }

        /// <summary>
        /// A connection ended or never came up. identifiedFor is how long it had been identified (null when
        /// identify never succeeded). Returns the wait before the next attempt.
        /// </summary>
        public TimeSpan OnConnectionEnded(TimeSpan? identifiedFor, double random01)
        {
            if (identifiedFor.HasValue && identifiedFor.Value >= _stableAfter) _failures = 0;
            TimeSpan wait = Delay(_failures, random01, _max);
            _failures++;
            return wait;
        }
    }
}
