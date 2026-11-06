using System;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Game
{
    /// <summary>
    /// The player's character level. The game has no level field in its mod API: it derives the level from the
    /// experience points, with the rule below, so this mirrors that rule in the game's own single precision.
    /// </summary>
    public static class Progression
    {
        public const int MaxExperience = 500000, MaxLevel = 25;

        /// <summary>The game's fractional level: 25 * sqrt(exp / 500000) + 1, kept between 1 and 25.</summary>
        public static float NativeLevel(int experience)
        {
            if (experience <= 0) return 1f;
            float fraction = (float)Math.Sqrt((float)experience / MaxExperience);
            float level = (float)(MaxLevel * fraction) + 1f;
            return level < 1f ? 1f : level > MaxLevel ? MaxLevel : level;
        }

        /// <summary>Experience where a level starts: the level rule solved for exp, 800 * (level - 1)^2.</summary>
        public static int LevelStart(int level)
        {
            double fraction = (level - 1) / (double)MaxLevel;
            return (int)Math.Round(fraction * fraction * MaxExperience);
        }

        /// <summary>
        /// Share of the current level's experience already earned, as the player's Stats panel shows it
        /// ("XP: 664 / 2400 27%"). The fractional part of NativeLevel is a different, non-linear number.
        /// </summary>
        public static double Progress(int experience, int level)
        {
            int start = LevelStart(level), next = LevelStart(level + 1);
            double progress = (experience - start) / (double)(next - start);
            return Math.Round(progress < 0d ? 0d : progress > 1d ? 1d : progress, 4);
        }

        /// <summary>The getPlayerProgression answer: the `character` track, or no tracks before the first reading.</summary>
        public static JArray Tracks(PlayerRecord r)
        {
            if (!r.HasExperience) return new JArray();
            int level = (int)NativeLevel(r.Experience);
            return new JArray(new JObject
            {
                ["key"] = "character",
                ["name"] = "Level",
                ["level"] = level,
                ["experience"] = r.Experience,
                ["progress"] = Progress(r.Experience, level),
            });
        }
    }
}
