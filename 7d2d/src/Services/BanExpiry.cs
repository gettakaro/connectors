using System;
using System.Globalization;

namespace Takaro.Services
{
    /// <summary>
    /// Converts between Takaro's absolute UTC ban timestamps and the server-local
    /// wall-clock deadlines used by Seven Days to Die's AdminBlacklist.
    /// </summary>
    public static class BanExpiry
    {
        public static bool TryCreateGameDeadline(
            string expiresAt,
            DateTimeOffset utcNow,
            TimeZoneInfo gameTimeZone,
            out DateTime gameDeadline,
            out string error
        )
        {
            gameDeadline = default(DateTime);
            error = string.Empty;

            if (
                string.IsNullOrWhiteSpace(expiresAt)
                || !DateTimeOffset.TryParse(
                    expiresAt,
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AllowWhiteSpaces,
                    out DateTimeOffset absoluteDeadline
                )
            )
            {
                error = "Invalid ban expiration timestamp";
                return false;
            }

            DateTimeOffset utcDeadline = absoluteDeadline.ToUniversalTime();
            if (utcDeadline <= utcNow.ToUniversalTime())
            {
                error = "Ban expiration must be in the future";
                return false;
            }

            DateTimeOffset localDeadline = TimeZoneInfo.ConvertTime(utcDeadline, gameTimeZone);
            gameDeadline = DateTime.SpecifyKind(localDeadline.DateTime, DateTimeKind.Unspecified);
            return true;
        }

        public static string ToTakaroUtc(DateTime gameDeadline, TimeZoneInfo gameTimeZone)
        {
            DateTime localWallClock = DateTime.SpecifyKind(gameDeadline, DateTimeKind.Unspecified);
            DateTime utcDeadline = TimeZoneInfo.ConvertTimeToUtc(localWallClock, gameTimeZone);
            return utcDeadline.ToString("o", CultureInfo.InvariantCulture);
        }
    }
}
