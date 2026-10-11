using System.Collections.Generic;
using Takaro.Persistence;

namespace Takaro.Services
{
    /// <summary>
    /// Keys a ban entry by the EOS id, like players. Takaro raises a Platform ID
    /// collision or creates a duplicate player for a gameId that is not an EOS id.
    /// </summary>
    public static class BanIdentity
    {
        private const string EosPrefix = "EOS_";
        private const string SteamPrefix = "Steam_";
        private const string XboxPrefix = "XBL_";

        /// <summary>
        /// Returns null when the entry cannot be keyed by an EOS id; the legacy
        /// integration skipped those entries as well.
        /// </summary>
        /// <param name="banId">The id the game stores the ban under.</param>
        /// <param name="crossplatformId">The EOS id the game resolved for this entry, or null.</param>
        /// <param name="nativeId">The platform id (Steam_/XBL_) of the entry, or null.</param>
        public static BanRecord ToRecord(
            string banId,
            string crossplatformId,
            string nativeId,
            string name,
            string reason,
            string expiresAt
        )
        {
            string eosId = FirstEos(banId, crossplatformId);
            if (eosId == null)
                return null;

            string gameId = eosId.Substring(EosPrefix.Length);
            string platformId = banId != null && !banId.StartsWith(EosPrefix) ? banId : nativeId;

            var record = new BanRecord
            {
                Id = banId,
                GameId = gameId,
                EpicOnlineServicesId = gameId,
                Name = string.IsNullOrEmpty(name) ? $"Player_{gameId}" : name,
                Reason = reason,
                ExpiresAt = expiresAt,
            };

            if (platformId != null && platformId.StartsWith(SteamPrefix))
                record.SteamId = platformId.Substring(SteamPrefix.Length);
            else if (platformId != null && platformId.StartsWith(XboxPrefix))
                record.XboxLiveId = platformId.Substring(XboxPrefix.Length);

            return record;
        }

        /// <summary>
        /// True when the ban mirror reports an entry stored under <paramref name="banId"/>
        /// as <paramref name="gameId"/>. Unban uses this so it lifts exactly the
        /// entries listBans reported, whichever platform id the game stored them under.
        /// </summary>
        public static bool IsReportedAs(string gameId, string banId, string crossplatformId)
        {
            BanRecord record = ToRecord(banId, crossplatformId, null, null, null, null);
            return record != null && record.GameId == gameId;
        }

        /// <summary>
        /// One record per gameId, first entry wins: a player banned under both an EOS
        /// and a native id is listed once. Entries without an EOS id (null) are dropped.
        /// </summary>
        public static List<BanRecord> DistinctByGameId(IEnumerable<BanRecord> records)
        {
            var distinct = new List<BanRecord>();
            var seenGameIds = new HashSet<string>();
            foreach (BanRecord record in records)
            {
                if (record != null && seenGameIds.Add(record.GameId))
                    distinct.Add(record);
            }
            return distinct;
        }

        private static string FirstEos(params string[] candidates)
        {
            foreach (string candidate in candidates)
            {
                if (candidate != null && candidate.StartsWith(EosPrefix))
                    return candidate;
            }
            return null;
        }
    }
}
