using System;
using System.Collections.Generic;

namespace Takaro.Commands.Contract
{
    /// <summary>
    /// Recognises a loaded PrismaCore mod. It registers the same command names as this
    /// connector, and both ship an assembly named LiteDB at different major versions, so
    /// running both is unsupported and the connector says so once at game start.
    /// </summary>
    public static class PrismaCoreDetection
    {
        public const string Warning =
            "PrismaCore is loaded next to the Takaro connector. Running both is unsupported: "
            + "they register the same console commands (resetlevel/rl, resetskillpoints/rs, isbloodmoon) "
            + "and each ships its own LiteDB assembly at a different major version. Remove one of them.";

        public static bool IsPrismaCore(
            string modName,
            string folderName,
            IEnumerable<string> assemblyNames
        )
        {
            if (Mentions(modName) || Mentions(folderName))
                return true;
            if (assemblyNames == null)
                return false;
            foreach (string assemblyName in assemblyNames)
            {
                if (Mentions(assemblyName))
                    return true;
            }
            return false;
        }

        private static bool Mentions(string value) =>
            !string.IsNullOrEmpty(value)
            && value.IndexOf("PrismaCore", StringComparison.OrdinalIgnoreCase) >= 0;
    }
}
