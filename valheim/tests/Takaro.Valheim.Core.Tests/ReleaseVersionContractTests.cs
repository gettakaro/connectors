using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ReleaseVersionContractTests
{
    [TestMethod]
    public void PluginMetadataSeparatesTheBepInExLoaderVersionFromTheReleaseVersion()
    {
        var entrypoint = ReadValheimFile("src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs");
        var project = ReadValheimFile("src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");

        Assert.AreEqual(2, CountOccurrences(entrypoint, "PluginVersion = TakaroBuildVersion.BepInExVersion"));
        Assert.AreEqual(2, CountOccurrences(entrypoint, "ReleaseVersion = TakaroBuildVersion.ReleaseVersion"));
        Assert.IsFalse(entrypoint.Contains("PluginVersion = \"0.1.0\"", StringComparison.Ordinal));
        StringAssert.Contains(project, "<TakaroValheimReleaseVersion Condition=");
        StringAssert.Contains(project, "<TakaroValheimBepInExVersion Condition=");
        StringAssert.Contains(project, "<Version>$(TakaroValheimReleaseVersion)</Version>");
        StringAssert.Contains(project, "<PackageVersion>$(TakaroValheimReleaseVersion)</PackageVersion>");
        StringAssert.Contains(project, "TakaroBuildVersion.g.cs");
    }

    [TestMethod]
    public void ReleaseScriptValidatesAndPropagatesSemanticVersionWithoutEditingSources()
    {
        var release = ReadValheimFile("scripts/build-release.sh");

        StringAssert.Contains(release, "source scripts/release-version.sh");
        StringAssert.Contains(release, "resolve_valheim_release_version \"$VERSION\"");
        StringAssert.Contains(release, "Invalid semantic version");
        StringAssert.Contains(release, "-p:TakaroValheimReleaseVersion=\"$VALHEIM_RELEASE_VERSION\"");
        StringAssert.Contains(release, "-p:TakaroValheimBepInExVersion=\"$VALHEIM_BEPINEX_VERSION\"");
        StringAssert.Contains(release, "-p:Version=\"$VALHEIM_RELEASE_VERSION\"");
        StringAssert.Contains(release, "-p:PackageVersion=\"$VALHEIM_RELEASE_VERSION\"");
        StringAssert.Contains(release, "-p:AssemblyVersion=\"$VALHEIM_ASSEMBLY_VERSION\"");
        StringAssert.Contains(release, "-p:FileVersion=\"$VALHEIM_ASSEMBLY_VERSION\"");
        StringAssert.Contains(release, "-p:InformationalVersion=\"$VALHEIM_RELEASE_VERSION\"");
        StringAssert.Contains(release, "-p:IncludeSourceRevisionInInformationalVersion=false");
        StringAssert.Contains(release, "manifest.json");
        Assert.IsFalse(release.Contains("sed -i", StringComparison.Ordinal));
    }

    [DataTestMethod]
    [DataRow("1.0.0", "1.0.0", "1.0.0.0")]
    [DataRow("7.8.9-rc.2+verify7", "7.8.9", "7.8.9.0")]
    [DataRow("2.3.4+build.5", "2.3.4", "2.3.4.0")]
    public void ResolvedBepInExVersionIsAcceptedBySystemVersion(
        string releaseVersion,
        string expectedBepInExVersion,
        string expectedAssemblyVersion)
    {
        foreach (var locale in ValidationLocales())
        {
            var (exitCode, loaderVersion, assemblyVersion, localeAfter) =
                RunVersionResolver(releaseVersion, locale);

            Assert.AreEqual(0, exitCode, $"{releaseVersion} under {locale}");
            Assert.AreEqual(expectedBepInExVersion, loaderVersion, locale);
            Assert.AreEqual(expectedAssemblyVersion, assemblyVersion, locale);
            Assert.AreEqual(locale, localeAfter, "The sourced resolver must not leak locale changes.");
            Assert.IsTrue(Version.TryParse(loaderVersion, out var parsed), loaderVersion);
            Assert.AreEqual(new Version(expectedBepInExVersion), parsed);
        }
    }

    [TestMethod]
    public void ReleaseVersionValidationRejectsNonAsciiIdentifiersBeforeBuildingInEveryAvailableLocale()
    {
        foreach (var locale in ValidationLocales())
        {
            foreach (var version in new[]
                     {
                         "1.2.3-é",
                         "1.2.3+crème",
                         "1.2.3-Ａ",
                         "1.2.3+rс.1"
                     })
            {
                var (resolverExitCode, _, _, localeAfter) = RunVersionResolver(version, locale);
                Assert.AreNotEqual(0, resolverExitCode, $"resolver accepted {version} under {locale}");
                Assert.AreEqual(locale, localeAfter, "The sourced resolver must not leak locale changes.");

                var testRoot = Directory.CreateTempSubdirectory("valheim-invalid-version-");
                var outputDirectory = Path.Combine(testRoot.FullName, "package");
                try
                {
                    var (releaseExitCode, standardOutput, standardError) =
                        RunRelease(version, locale, outputDirectory);

                    Assert.AreEqual(2, releaseExitCode, $"build-release accepted {version} under {locale}");
                    StringAssert.Contains(standardError, "Invalid semantic version", version);
                    Assert.IsFalse(
                        standardOutput.Contains("Building Valheim connector", StringComparison.Ordinal),
                        $"build started for invalid version {version} under {locale}");
                    Assert.IsFalse(
                        Directory.Exists(outputDirectory),
                        $"package output was created for invalid version {version} under {locale}");
                }
                finally
                {
                    testRoot.Delete(recursive: true);
                }
            }
        }
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

    [TestMethod]
    public void ReleaseScriptRejectsVersionsOutsideDotNetNumericMetadataBounds()
    {
        foreach (var version in new[]
                 {
                     "65535.1.1",
                     "1.65535.1",
                     "1.1.65535",
                     "999999999999999999999999.1.1"
                 })
        {
            var (exitCode, standardError) = RunRelease(version);

            Assert.AreEqual(2, exitCode, version);
            StringAssert.Contains(standardError, "cannot exceed 65534", version);
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
        var outputDirectory = Path.Combine(
            Path.GetTempPath(),
            $"valheim-invalid-version-test-{Guid.NewGuid():N}");
        var (exitCode, _, standardError) = RunRelease(version, null, outputDirectory);
        return (exitCode, standardError);
    }

    private static (int ExitCode, string StandardOutput, string StandardError) RunRelease(
        string version,
        string? locale,
        string outputDirectory)
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
        startInfo.ArgumentList.Add(outputDirectory);
        if (locale is not null)
        {
            startInfo.Environment["LC_ALL"] = locale;
        }

        using var process = Process.Start(startInfo)!;
        var standardOutput = process.StandardOutput.ReadToEnd();
        var standardError = process.StandardError.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, standardOutput, standardError);
    }

    private static (
        int ExitCode,
        string LoaderVersion,
        string AssemblyVersion,
        string LocaleAfter) RunVersionResolver(string version, string locale)
    {
        var valheimDirectory = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../"));
        var resolverScript = Path.Combine(valheimDirectory, "scripts/release-version.sh");
        var startInfo = new ProcessStartInfo
        {
            FileName = "/usr/bin/env",
            WorkingDirectory = valheimDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("bash");
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(
            "source \"$1\"; " +
            "if resolve_valheim_release_version \"$2\"; then status=0; else status=$?; fi; " +
            "printf 'loader=%s\\nassembly=%s\\nlocale=%s\\n' " +
            "\"${VALHEIM_BEPINEX_VERSION-}\" \"${VALHEIM_ASSEMBLY_VERSION-}\" \"${LC_ALL-}\"; " +
            "exit \"$status\"");
        startInfo.ArgumentList.Add("valheim-release-version-test");
        startInfo.ArgumentList.Add(resolverScript);
        startInfo.ArgumentList.Add(version);
        startInfo.Environment["LC_ALL"] = locale;

        using var process = Process.Start(startInfo)!;
        var lines = process.StandardOutput.ReadToEnd()
            .Split('\n', StringSplitOptions.RemoveEmptyEntries);
        process.StandardError.ReadToEnd();
        process.WaitForExit();
        return (
            process.ExitCode,
            ReadValue(lines, "loader="),
            ReadValue(lines, "assembly="),
            ReadValue(lines, "locale="));
    }

    private static IReadOnlyList<string> ValidationLocales()
    {
        var locales = new List<string> { "C" };
        foreach (var candidate in new[] { "C.UTF-8", "en_US.UTF-8" })
        {
            if (LocaleIsAvailable(candidate))
            {
                locales.Add(candidate);
            }
        }

        return locales;
    }

    private static bool LocaleIsAvailable(string locale)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = "/usr/bin/env",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("locale");
        startInfo.ArgumentList.Add("charmap");
        startInfo.Environment["LC_ALL"] = locale;

        using var process = Process.Start(startInfo)!;
        process.StandardOutput.ReadToEnd();
        process.StandardError.ReadToEnd();
        process.WaitForExit();
        return process.ExitCode == 0;
    }

    private static string ReadValue(IEnumerable<string> lines, string prefix)
    {
        var line = lines.Single(value => value.StartsWith(prefix, StringComparison.Ordinal));
        return line[prefix.Length..];
    }
}
