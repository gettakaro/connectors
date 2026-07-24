using System;

namespace Takaro.Services
{
    /// <summary>
    /// Keeps the last sampled position readable briefly after disconnect so
    /// Takaro can enrich the lifecycle event without exposing stale offline
    /// player locations indefinitely.
    /// </summary>
    public static class PlayerLocationReadWindow
    {
        public static readonly TimeSpan DisconnectedGracePeriod = TimeSpan.FromSeconds(30);

        public static bool IsReadable(bool online, DateTime lastSeenUtc, DateTime utcNow)
        {
            return IsReadable(online, lastSeenUtc, utcNow, TimeZoneInfo.Local);
        }

        public static bool IsReadable(
            bool online,
            DateTime lastSeenUtc,
            DateTime utcNow,
            TimeZoneInfo localTimeZone
        )
        {
            if (online)
                return true;

            DateTime normalizedLastSeenUtc = ToUtc(lastSeenUtc, localTimeZone);
            DateTime normalizedNowUtc = ToUtc(utcNow, localTimeZone);
            TimeSpan age = normalizedNowUtc - normalizedLastSeenUtc;
            return age >= TimeSpan.Zero && age <= DisconnectedGracePeriod;
        }

        private static DateTime ToUtc(DateTime value, TimeZoneInfo localTimeZone)
        {
            if (value.Kind == DateTimeKind.Utc)
                return value;
            if (value.Kind == DateTimeKind.Local)
                return value.ToUniversalTime();

            return TimeZoneInfo.ConvertTimeToUtc(value, localTimeZone);
        }
    }
}
