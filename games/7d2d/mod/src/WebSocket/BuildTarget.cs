using System;
using System.Reflection;
using System.Text.RegularExpressions;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Which game build this mod was compiled for, and whether the running server is that build.
    /// The release build stamps the catalog revision (for example <c>3.4.0.b9</c>) into the
    /// assembly as <c>[AssemblyMetadata("TakaroGameBuild", ...)]</c>; a local build without it
    /// reports Unknown. Pure .NET so the contract harness can drive it.
    /// </summary>
    public static class BuildTarget
    {
        public const string MetadataKey = "TakaroGameBuild";
        public const string DownloadPage = "https://takaro.io/connectors/7d2d";

        public enum Match
        {
            /// <summary>The built or the running build is not known.</summary>
            Unknown,

            /// <summary>The server runs exactly the build this mod was compiled for.</summary>
            Exact,

            /// <summary>Another build with the same game API: expected to work, not checked.</summary>
            SameApi,

            /// <summary>V 3.3.0 changed player data; this build cannot read inventories there.</summary>
            WrongApi,
        }

        // "V 3.4.0 (b9)", as Constants.cVersionInformation.LongString and the console print it.
        private static readonly Regex RuntimeVersion = new Regex(
            @"V\s*(\d+(?:\.\d+)*)\s*\(\s*(b\d+)\s*\)",
            RegexOptions.CultureInvariant
        );

        private static readonly Regex Revision = new Regex(
            @"^(\d+)\.(\d+)(?:\.\d+)*\.b\d+$",
            RegexOptions.CultureInvariant
        );

        /// <summary>The catalog revision this mod was built for, or null.</summary>
        public static string Built { get; private set; }

        /// <summary>The running server's build in the same form, or null.</summary>
        public static string Running { get; private set; }

        public static Match Result { get; private set; }

        public static void Configure(string built, string runtimeLongString)
        {
            Built = Revision.IsMatch(built?.Trim() ?? "") ? built.Trim() : null;
            Running = ParseRuntime(runtimeLongString);
            Result = Compare(Built, Running);
        }

        /// <summary>The revision stamped into this assembly by the release build, or null.</summary>
        public static string ReadStamp(Assembly assembly)
        {
            if (assembly == null)
                return null;
            foreach (
                object attribute in assembly.GetCustomAttributes(
                    typeof(AssemblyMetadataAttribute),
                    false
                )
            )
            {
                var metadata = (AssemblyMetadataAttribute)attribute;
                if (metadata.Key == MetadataKey && !string.IsNullOrEmpty(metadata.Value))
                    return metadata.Value;
            }
            return null;
        }

        /// <summary>"V 3.4.0 (b9)" -> "3.4.0.b9"; null when it is not in that form.</summary>
        public static string ParseRuntime(string longString)
        {
            if (string.IsNullOrEmpty(longString))
                return null;
            System.Text.RegularExpressions.Match match = RuntimeVersion.Match(longString);
            return match.Success ? match.Groups[1].Value + "." + match.Groups[2].Value : null;
        }

        /// <summary>1 before V 3.3.0, 2 from it: the same split as sevend2d_api_defines.</summary>
        public static int ApiGeneration(string revision)
        {
            System.Text.RegularExpressions.Match match = Revision.Match(revision ?? "");
            if (!match.Success)
                return 0;
            int major = int.Parse(match.Groups[1].Value);
            int minor = int.Parse(match.Groups[2].Value);
            return major > 3 || (major == 3 && minor >= 3) ? 2 : 1;
        }

        public static Match Compare(string built, string running)
        {
            if (built == null || running == null)
                return Match.Unknown;
            if (string.Equals(built, running, StringComparison.Ordinal))
                return Match.Exact;
            return ApiGeneration(built) == ApiGeneration(running) ? Match.SameApi : Match.WrongApi;
        }

        /// <summary>"3.4.0.b9" -> "V 3.4.0 b9", the way the README and the release name builds.</summary>
        public static string Display(string revision)
        {
            if (revision == null)
                return "an unknown build";
            int b = revision.LastIndexOf(".b", StringComparison.Ordinal);
            return b < 0
                ? "V " + revision
                : "V " + revision.Substring(0, b) + " " + revision.Substring(b + 1);
        }

        /// <summary>
        /// The connector version Takaro shows, with the game build as semver build metadata
        /// ("0.7.1" -> "0.7.1+3.4.0.b9"), so support can see which zip a server runs.
        /// </summary>
        public static string VersionWithBuild(string connectorVersion)
        {
            string version = connectorVersion?.Trim();
            if (string.IsNullOrEmpty(version) || Built == null || version.IndexOf('+') >= 0)
                return version;
            return version + "+" + Built;
        }

        /// <summary>Null when the build fits; otherwise the line to log, once, at startup.</summary>
        public static string StartupProblem()
        {
            switch (Result)
            {
                case Match.WrongApi:
                    return WrongBuildMessage();
                case Match.SameApi:
                    return $"This Takaro mod was built for 7 Days to Die {Display(Built)}, but the server runs "
                        + $"{Display(Running)}. It is expected to work but was not checked on this build. "
                        + $"If there is a zip for {Display(Running)} on {DownloadPage}, install that one.";
                default:
                    return null;
            }
        }

        public static string WrongBuildMessage()
        {
            return $"Wrong Takaro mod build: this zip is for 7 Days to Die {Display(Built)}, but the server runs "
                + $"{Display(Running)}. Player inventories cannot be read on this build. Install the zip for "
                + $"{Display(Running)} from {DownloadPage} (takaro-7d2d-mod-{Running}-<version>.zip).";
        }
    }
}
