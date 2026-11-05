using System.Collections.Generic;
using System.Globalization;

namespace Takaro.Commands.Contract
{
    /// <summary>
    /// The PrismaCore output contract of the progression family (resetlevel/rl,
    /// resetskillpoints/rs): names, argument rules and every line, verbatim. Game-free, so
    /// the contract harness asserts it; Commands/ProgressionCommands.cs applies it.
    /// </summary>
    public static class ProgressionContract
    {
        public sealed class Spec
        {
            public string[] Names;
            public string ValueName;
            public string ValueLabel;
            public int Minimum;
            public string ResetName;
            public string SuccessFormat;

            public string OfflineLine =>
                "ERR: Player is offline. Player MUST be online for " + ResetName + ".";

            public string SuccessLine(int value) =>
                string.Format(CultureInfo.InvariantCulture, SuccessFormat, value);
        }

        public static readonly Spec ResetLevel = new Spec
        {
            Names = new[] { "resetlevel", "rl" },
            ValueName = "level",
            ValueLabel = "Level",
            Minimum = 1,
            ResetName = "level reset",
            SuccessFormat = "You have reset the level of this player to level {0}.",
        };

        public static readonly Spec ResetSkillPoints = new Spec
        {
            Names = new[] { "resetskillpoints", "rs" },
            ValueName = "count",
            ValueLabel = "Count",
            Minimum = 0,
            ResetName = "skillpoints reset",
            SuccessFormat = "You have reset the skillpoints for this player to {0}.",
        };

        /// <summary>
        /// Checks <c>&lt;player&gt; &lt;value&gt;</c>. Returns the error line, or null with the
        /// player argument and the value set. The player is resolved only after this passes.
        /// </summary>
        public static string Validate(
            Spec spec,
            IList<string> args,
            out string playerId,
            out int value
        )
        {
            playerId = null;
            value = 0;
            if (args.Count != 2)
                return "ERR: Wrong number of arguments, expected 2, found " + args.Count;
            if (
                !int.TryParse(
                    args[1],
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out value
                )
            )
                return "ERR: Invalid "
                    + spec.ValueName
                    + " parameter "
                    + args[1]
                    + ". Must be a valid integer";
            if (value < spec.Minimum)
                return "ERR: "
                    + spec.ValueLabel
                    + " must be "
                    + spec.Minimum.ToString(CultureInfo.InvariantCulture)
                    + " or greater.";
            playerId = args[0];
            return null;
        }
    }
}
