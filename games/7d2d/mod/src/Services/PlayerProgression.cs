using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using Newtonsoft.Json;

namespace Takaro.Services
{
    /// <summary>One Progression Track of a getPlayerProgression answer.</summary>
    public class TakaroProgressionTrack
    {
        [JsonProperty("key")]
        public string Key { get; set; }

        [JsonProperty("name", NullValueHandling = NullValueHandling.Ignore)]
        public string Name { get; set; }

        [JsonProperty("level")]
        public int Level { get; set; }

        [JsonProperty("experience", NullValueHandling = NullValueHandling.Ignore)]
        public long? Experience { get; set; }

        [JsonProperty("progress", NullValueHandling = NullValueHandling.Ignore)]
        public double? Progress { get; set; }
    }

    /// <summary>
    /// Builds a getPlayerProgression answer from values read off the game. Game-free, so the
    /// contract harness asserts it; StateMirror does the reading. One invalid track makes
    /// Takaro drop the whole reading, and modded progression.xml files can bring any name,
    /// so what Takaro would refuse is left out here.
    /// </summary>
    public static class PlayerProgression
    {
        public const string CharacterKey = "character";
        public const string CharacterName = "Level";
        public const int MaxTracks = 200;
        public const int MaxTextLength = 128;

        private static readonly Regex ValidKey = new Regex(
            @"^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9-]*)*$",
            RegexOptions.CultureInvariant
        );

        public static bool IsValidKey(string key) =>
            !string.IsNullOrEmpty(key)
            && key.Length <= MaxTextLength
            && key != CharacterKey
            && ValidKey.IsMatch(key);

        /// <summary>
        /// Experience earned over the character's life: the cost of every level already
        /// passed plus what is earned toward the next one. The game only keeps the amount
        /// left to the next level, which starts over at each level.
        /// </summary>
        /// <param name="costOfLevel">Experience the game asks to leave a level.</param>
        public static long TotalExperience(
            int level,
            int expToNextLevel,
            Func<int, int> costOfLevel
        )
        {
            long total = 0;
            for (int passed = 1; passed < level; passed++)
                total += costOfLevel(passed);
            return total + Math.Max(0, costOfLevel(level) - Math.Max(0, expToNextLevel));
        }

        /// <summary>Fraction of the current level earned, or null when the game has no answer.</summary>
        public static double? LevelProgress(int expToNextLevel, int expForNextLevel)
        {
            if (expForNextLevel <= 0)
                return null;
            double earned = 1d - (double)expToNextLevel / expForNextLevel;
            return Math.Min(1d, Math.Max(0d, earned));
        }

        /// <summary>The character level first, then <paramref name="others"/> in their order.</summary>
        public static List<TakaroProgressionTrack> Build(
            int level,
            long experience,
            double? progress,
            IEnumerable<TakaroProgressionTrack> others
        )
        {
            var tracks = new List<TakaroProgressionTrack>
            {
                new TakaroProgressionTrack
                {
                    Key = CharacterKey,
                    Name = CharacterName,
                    Level = level,
                    Experience = experience,
                    Progress = progress,
                },
            };

            var seen = new HashSet<string> { CharacterKey };
            foreach (TakaroProgressionTrack track in others)
            {
                if (tracks.Count >= MaxTracks)
                    break;
                if (!IsValidKey(track.Key) || !seen.Add(track.Key))
                    continue;
                if (track.Name != null && track.Name.Length > MaxTextLength)
                    track.Name = track.Name.Substring(0, MaxTextLength);
                tracks.Add(track);
            }
            return tracks;
        }
    }
}
