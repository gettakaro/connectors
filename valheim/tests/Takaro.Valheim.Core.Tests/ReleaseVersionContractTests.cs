using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ReleaseVersionContractTests
{
    [TestMethod]
    public void PluginMetadataUsesTheMsbuildGeneratedReleaseVersion()
    {
        var entrypoint = ReadValheimFile("src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs");
        var project = ReadValheimFile("src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");

        Assert.AreEqual(2, CountOccurrences(entrypoint, "PluginVersion = TakaroBuildVersion.Value"));
        Assert.IsFalse(entrypoint.Contains("PluginVersion = \"0.1.0\"", StringComparison.Ordinal));
        StringAssert.Contains(project, "<TakaroValheimPluginVersion Condition=");
        StringAssert.Contains(project, "<Version>$(TakaroValheimPluginVersion)</Version>");
        StringAssert.Contains(project, "<PackageVersion>$(TakaroValheimPluginVersion)</PackageVersion>");
        StringAssert.Contains(project, "TakaroBuildVersion.g.cs");
    }

    [TestMethod]
    public void ReleaseScriptValidatesAndPropagatesSemanticVersionWithoutEditingSources()
    {
        var release = ReadValheimFile("scripts/build-release.sh");

        StringAssert.Contains(release, "SEMVER_PATTERN=");
        StringAssert.Contains(release, "Invalid semantic version");
        StringAssert.Contains(release, "-p:TakaroValheimPluginVersion=\"$VERSION\"");
        StringAssert.Contains(release, "-p:Version=\"$VERSION\"");
        StringAssert.Contains(release, "-p:PackageVersion=\"$VERSION\"");
        StringAssert.Contains(release, "-p:AssemblyVersion=\"$ASSEMBLY_VERSION\"");
        StringAssert.Contains(release, "-p:FileVersion=\"$ASSEMBLY_VERSION\"");
        StringAssert.Contains(release, "-p:InformationalVersion=\"$VERSION\"");
        StringAssert.Contains(release, "-p:IncludeSourceRevisionInInformationalVersion=false");
        StringAssert.Contains(release, "manifest.json");
        Assert.IsFalse(release.Contains("sed -i", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ReleaseScriptRejectsMalformedAndLeadingZeroSemanticVersionsBeforeBuilding()
    {
        foreach (var version in new[]
                 {
                     "01.2.3",
                     "1.02.3",
                     "1.2.03",
                     "1.2",
                     "v1.2.3",
                     "1.2.3-01",
                     "1.2.3;touch-pwned"
                 })
        {
            var (exitCode, standardError) = RunRelease(version);

            Assert.AreEqual(2, exitCode, version);
            StringAssert.Contains(standardError, "Invalid semantic version", version);
        }
    }

    private static int CountOccurrences(string value, string marker)
    {
        var count = 0;
        var offset = 0;
        while ((offset = value.IndexOf(marker, offset, StringComparison.Ordinal)) >= 0)
        {
            count++;
            offset += marker.Length;
        }

        return count;
    }

    private static string ReadValheimFile(string relativePath)
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../",
            relativePath));

        return File.ReadAllText(path);
    }

    private static (int ExitCode, string StandardError) RunRelease(string version)
    {
        var valheimDirectory = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../"));
        var releaseScript = Path.Combine(valheimDirectory, "scripts/build-release.sh");
        var startInfo = new ProcessStartInfo
        {
            FileName = "/usr/bin/env",
            WorkingDirectory = valheimDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("bash");
        startInfo.ArgumentList.Add(releaseScript);
        startInfo.ArgumentList.Add(version);
        startInfo.ArgumentList.Add(Path.Combine(Path.GetTempPath(), "valheim-invalid-version-test"));

        using var process = Process.Start(startInfo)!;
        process.StandardOutput.ReadToEnd();
        var standardError = process.StandardError.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, standardError);
    }
}
