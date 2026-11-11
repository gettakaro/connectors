using System;

namespace Takaro.WebSocket
{
    /// <summary>
    /// How long to wait before the next connection attempt (gettakaro/connectors#858).
    ///
    /// A connection that was confirmed and stayed up is retried almost at once:
    /// a cut mid-session is harmless if the connector is back before Takaro's
    /// reachability check notices. Only consecutive failures back off,
    /// exponentially from the configured base, capped at MaxSeconds. Every delay
    /// is jittered so a Takaro deploy does not bring every server back in the
    /// same instant.
    ///
    /// No game types and no clock, so the contract harness covers it.
    /// </summary>
    public static class ReconnectPolicy
    {
        // Kept to half a second: the TLS handshake and the identify round trip
        // add one to one and a half seconds before Takaro sees the server again,
        // and the whole gap has to stay under two.
        public const double FastRetryMinSeconds = 0.1;
        public const double FastRetryJitterSeconds = 0.4;
        public const int MaxSeconds = 60;

        /// <summary>
        /// The delay before the next attempt.
        /// </summary>
        /// <param name="failureStreak">
        /// Attempts that failed since the last stable connection; 0 means the
        /// connection that just closed was confirmed and stable (or this is the
        /// first attempt after a restart or config change).
        /// </param>
        /// <param name="baseSeconds">Config.xml's ReconnectIntervalSeconds.</param>
        /// <param name="random">A value in [0, 1).</param>
        public static TimeSpan NextDelay(int failureStreak, int baseSeconds, double random)
        {
            random = Math.Min(Math.Max(random, 0), 1);

            if (failureStreak <= 0)
                return TimeSpan.FromSeconds(
                    FastRetryMinSeconds + FastRetryJitterSeconds * random
                );

            int clampedBase = Math.Min(Math.Max(1, baseSeconds), MaxSeconds);
            int exponent = Math.Min(16, failureStreak - 1);
            double ceiling = Math.Min(clampedBase * Math.Pow(2, exponent), MaxSeconds);

            // Equal jitter: at least half the ceiling, so the streak still grows.
            return TimeSpan.FromSeconds(ceiling / 2 + ceiling / 2 * random);
        }

        /// <summary>
        /// Whether a closed connection resets the failure streak: it must have
        /// been confirmed (identify accepted) and stayed up for stableSeconds.
        /// A peer that accepts and then drops every socket keeps backing off.
        /// </summary>
        public static bool ClosedSessionWasStable(
            bool wasConfirmed,
            int uptimeSeconds,
            int stableSeconds
        )
        {
            return wasConfirmed && uptimeSeconds >= stableSeconds;
        }
    }
}
