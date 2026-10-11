using System;
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

        /// <summary>
        /// Indexes of the entries that listBans reports under <paramref name="gameId"/>.
        /// Each entry is the id the game stores the ban under and the EOS id it resolved
        /// for it, or null.
        /// </summary>
        public static List<int> SelectEntriesToLift(
            string gameId,
            IList<KeyValuePair<string, string>> entries
        )
        {
            var selected = new List<int>();
            for (int index = 0; index < entries.Count; index++)
            {
                if (IsReportedAs(gameId, entries[index].Key, entries[index].Value))
                    selected.Add(index);
            }
            return selected;
        }

        /// <summary>
        /// Names the stores an unban lifted something from, or null when it lifted nothing.
        /// A player can sit in both stores, so one store being empty never ends the unban.
        /// </summary>
        public static string UnbanMethod(bool blockedPlayerListLifted, int blacklistEntriesLifted)
        {
            if (blockedPlayerListLifted && blacklistEntriesLifted > 0)
                return "BlockedPlayerList+AdminTools.Blacklist";
            if (blockedPlayerListLifted)
                return "BlockedPlayerList";
            return blacklistEntriesLifted > 0 ? "AdminTools.Blacklist" : null;
        }

        /// <summary>
        /// One ban as a game store holds it, before it is keyed by the EOS id.
        /// </summary>
        public sealed class BanSource
        {
            public string BanId;
            public string CrossplatformId;
            public string NativeId;
            public string Name;
            public string Reason;
            public string ExpiresAt;
        }

        /// <summary>
        /// The ban list Takaro sees: both stores merged, Blacklist first, one record per
        /// player. <paramref name="onSkipped"/> receives the id of each entry that has no
        /// EOS id.
        /// </summary>
        public static List<BanRecord> Capture(
            IEnumerable<BanSource> blacklist,
            IEnumerable<BanSource> blockedPlayerList,
            Action<string> onSkipped
        )
        {
            var candidates = new List<BanRecord>();
            foreach (BanSource source in Concat(blacklist, blockedPlayerList))
            {
                BanRecord record = ToRecord(
                    source.BanId,
                    source.CrossplatformId,
                    source.NativeId,
                    source.Name,
                    source.Reason,
                    source.ExpiresAt
                );
                if (record == null)
                    onSkipped?.Invoke(source.BanId);
                else
                    candidates.Add(record);
            }
            return DistinctByGameId(candidates);
        }

        private static IEnumerable<BanSource> Concat(
            IEnumerable<BanSource> first,
            IEnumerable<BanSource> second
        )
        {
            foreach (BanSource source in first)
                yield return source;
            foreach (BanSource source in second)
                yield return source;
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
