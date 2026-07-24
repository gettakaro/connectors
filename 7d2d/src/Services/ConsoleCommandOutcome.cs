using System;
using System.IO;

namespace Takaro.Services
{
    public sealed class ConsoleCommandOutcome
    {
        private static readonly string[] FailurePrefixes =
        {
            "*** ERROR:",
            "Wrong number of arguments",
            "Invalid value for",
        };
        public const int MaxErrorMessageLength = 512;

        public string RawResult { get; private set; }
        public bool Success { get; private set; }
        public string ErrorMessage { get; private set; }

        private ConsoleCommandOutcome() { }

        public static ConsoleCommandOutcome FromRawResult(string rawResult)
        {
            string errorMessage = null;
            using (var reader = new StringReader(rawResult ?? string.Empty))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    bool isFailure = false;
                    foreach (string prefix in FailurePrefixes)
                    {
                        if (!line.StartsWith(prefix, StringComparison.Ordinal))
                            continue;
                        isFailure = true;
                        break;
                    }

                    if (!isFailure)
                        continue;

                    errorMessage =
                        line.Length <= MaxErrorMessageLength
                            ? line
                            : line.Substring(0, MaxErrorMessageLength);
                    break;
                }
            }

            return new ConsoleCommandOutcome
            {
                RawResult = rawResult,
                Success = errorMessage == null,
                ErrorMessage = errorMessage,
            };
        }
    }
}
