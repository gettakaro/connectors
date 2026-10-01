using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ReleasePackageContractTests
{
    private const string Version = "2.0.0-rc.1+package";

    // The three BepInEx-shaped numbers a packaged manifest states, each a different fact:
    // what [BepInPlugin] declares, the Thunderstore pack the target pins, and the loader
    // assembly's own version.
    private const string PluginVersion = "2.0.0";
    private const string PackVersion = "5.4.2350";
    private const string LoaderVersion = "5.4.23.5";

    [TestMethod]
    public void ValidServerPluginFixturePasses()
    {
        using var fixture = CreateFixture();

        var result = RunHarness(fixture.Path);

        Assert.AreEqual(0, result.ExitCode, result.StandardError);
    }

    [DataTestMethod]
    [DataRow("missing-server-dll")]
    [DataRow("missing-core-dll")]
    [DataRow("wrong-server-role")]
    [DataRow("companion-dll-in-server")]
    [DataRow("config-in-server")]
    [DataRow("pdb-in-server")]
    [DataRow("deps-in-server")]
    [DataRow("host-dll-in-server")]
    [DataRow("jotunn-in-server")]
    [DataRow("extra-top-level-entry")]
    [DataRow("second-plugin-archive")]
    [DataRow("product-version-mismatch")]
    [DataRow("companion-protocol-in-manifest")]
    [DataRow("bepinex-version-equals-plugin-version")]
    [DataRow("pack-version-floating")]
    [DataRow("missing-plugin-version")]
    public void InvalidPackageFixturesAreRejected(string mutation)
    {
        using var fixture = CreateFixture(mutation);

        var result = RunHarness(fixture.Path);

        Assert.AreNotEqual(0, result.ExitCode, mutation);
        Assert.IsFalse(string.IsNullOrWhiteSpace(result.StandardError), mutation);
    }

    [TestMethod]
    public void ReleaseScriptsAndWorkflowPublishOnlyTheServerPluginArchive()
    {
        var harness = ReadValheimFile("tests/release-package-behavior.sh");
        var release = ReadValheimFile("scripts/build-release.sh");
        var workflow = ReadRepositoryFile(".github/workflows/valheim.yml");
        var game = ReadRepositoryFile("catalog/valheim/game.json");

        // The release script spells no archive name: it takes it from the resolved target,
        // so a re-pin renames the artifact on its own.
        StringAssert.Contains(release, "VALHEIM_ARTIFACT_SERVER_PLUGIN");
        Assert.IsFalse(
            release.Contains("takaro-valheim-plugin", StringComparison.Ordinal),
            "the release script must not hard-code an archive name the catalog owns.");
        Assert.IsFalse(release.Contains("companion", StringComparison.OrdinalIgnoreCase));
        Assert.IsFalse(release.Contains("CLIENT_", StringComparison.Ordinal));
        Assert.AreEqual(
            1,
            release.Split("dotnet publish ", StringSplitOptions.None).Length - 1,
            "the release builds exactly one project.");
        StringAssert.Contains(release, "mod/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");

        // The harness finds the one plugin archive by name, whatever the target is.
        StringAssert.Contains(harness, "takaro-valheim-plugin");
        Assert.IsFalse(harness.Contains("TakaroValheimCompanion", StringComparison.Ordinal));

        // The pattern and the legacy alias are declared in one place: the game record.
        StringAssert.Contains(game, "takaro-valheim-plugin-{target}-{version}.zip");
        StringAssert.Contains(game, "\"takaro-valheim-plugin.zip\"");
        StringAssert.Contains(game, "server-plugin");
        Assert.IsFalse(game.Contains("companion", StringComparison.OrdinalIgnoreCase));

        StringAssert.Contains(release, "SOURCE_DATE_EPOCH");
        StringAssert.Contains(release, "zip -X");
        StringAssert.Contains(release, "LC_ALL=C sort");
        StringAssert.Contains(release, "release-package-behavior.sh");

        // One publisher for every connector: the workflow hands the role to the shared
        // release workflow instead of naming assets itself.
        StringAssert.Contains(workflow, "./.github/workflows/connector-release.yml");
        StringAssert.Contains(workflow, "connector: valheim");
    }

    [TestMethod]
    public void TestWorkflowInstallsReleaseHarnessDependencies()
    {
        var workflow = ReadRepositoryFile(".github/workflows/valheim.yml");
        var testJobStart = workflow.IndexOf("  test:\n", StringComparison.Ordinal);
        var releaseJobStart = workflow.IndexOf("  release:\n", StringComparison.Ordinal);

        Assert.IsTrue(testJobStart >= 0, "Valheim workflow is missing the test job.");
        Assert.IsTrue(
            releaseJobStart > testJobStart,
            "Valheim workflow is missing the release job after the test job.");

        // The C# suite shells out to both bash harnesses, so the test job installs what
        // they need before anything is released.
        var testJob = workflow[testJobStart..releaseJobStart];
        StringAssert.Contains(testJob, "sudo apt-get install -y ripgrep");
    }

    [TestMethod]
    public void DeterministicPackageUpgradeInstructionsClearBepInExTypeCache()
    {
        var release = ReadValheimFile("scripts/build-release.sh");
        const string cachePath = "BepInEx/cache/chainloader_typeloader.dat";

        Assert.AreEqual(
            1,
            release.Split(cachePath, StringSplitOptions.None).Length - 1,
            "The packaged server README must invalidate BepInEx's metadata cache.");
        StringAssert.Contains(release, "before restarting");
    }

    private static TemporaryDirectory CreateFixture(string? mutation = null)
    {
        var fixture = new TemporaryDirectory();
        var stage = Path.Combine(fixture.Path, "server");
        var server = Path.Combine(stage, "TakaroValheim");
        Directory.CreateDirectory(server);

        Write(Path.Combine(server, "TakaroValheim.dll"), "server fixture");
        Write(Path.Combine(server, "Takaro.Valheim.Core.dll"), "core fixture");
        Write(Path.Combine(server, "README.txt"), "server install fixture");
        WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server");

        switch (mutation)
        {
            case null:
                break;
            case "missing-server-dll":
                File.Delete(Path.Combine(server, "TakaroValheim.dll"));
                break;
            case "missing-core-dll":
                File.Delete(Path.Combine(server, "Takaro.Valheim.Core.dll"));
                break;
            case "wrong-server-role":
                WriteManifest(Path.Combine(server, "manifest.json"), "graphical-client");
                break;
            case "companion-dll-in-server":
                Write(Path.Combine(server, "Takaro.Valheim.Companion.Protocol.dll"), "retired role");
                break;
            case "config-in-server":
                Write(Path.Combine(server, "com.takaro.valheim.cfg"), "registrationToken=secret");
                break;
            case "pdb-in-server":
                Write(Path.Combine(server, "TakaroValheim.pdb"), "debug");
                break;
            case "deps-in-server":
                Write(Path.Combine(server, "TakaroValheim.deps.json"), "{}");
                break;
            case "host-dll-in-server":
                Write(Path.Combine(server, "BepInEx.dll"), "host");
                break;
            case "jotunn-in-server":
                Write(Path.Combine(server, "Jotunn.dll"), "host");
                break;
            case "extra-top-level-entry":
                Write(Path.Combine(stage, "Other", "Other.dll"), "stray");
                break;
            case "second-plugin-archive":
                ZipFile.CreateFromDirectory(
                    stage,
                    Path.Combine(fixture.Path, "takaro-valheim-plugin-linux-1.0.16-2.0.0.zip"),
                    CompressionLevel.NoCompression,
                    includeBaseDirectory: false);
                break;
            case "product-version-mismatch":
                WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server", version: "2.0.1");
                break;
            case "companion-protocol-in-manifest":
                WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server", withProtocol: true);
                break;
            // The loader version, pack version and plugin version are independent fields.
            case "bepinex-version-equals-plugin-version":
                WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server", loaderVersion: PluginVersion);
                break;
            case "pack-version-floating":
                WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server", packVersion: "latest");
                break;
            case "missing-plugin-version":
                WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server", pluginVersion: null);
                break;
            default:
                Assert.Fail($"Unknown fixture mutation {mutation}.");
                break;
        }

        ZipFile.CreateFromDirectory(
            stage,
            Path.Combine(fixture.Path, "takaro-valheim-plugin.zip"),
            CompressionLevel.NoCompression,
            includeBaseDirectory: false);
        return fixture;
    }

    private static void WriteManifest(
        string path,
        string role,
        string version = Version,
        bool withProtocol = false,
        string? pluginVersion = PluginVersion,
        string packVersion = PackVersion,
        string loaderVersion = LoaderVersion)
    {
        var manifest = new Dictionary<string, object?>
        {
            ["name"] = "TakaroValheim",
            ["productVersion"] = version,
            ["bepInExPack"] = new
            {
                @namespace = "denikson",
                name = "BepInExPack_Valheim",
                version = packVersion
            },
            ["bepInExVersion"] = loaderVersion,
            ["processRole"] = role
        };
        if (withProtocol)
        {
            manifest["protocol"] = new { minimum = 2, current = 2, maximum = 2 };
        }

        if (pluginVersion is not null)
        {
            manifest["pluginVersion"] = pluginVersion;
        }

        Write(path, JsonSerializer.Serialize(manifest));
    }

    private static void Write(string path, string contents)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, contents);
    }

    private static ProcessResult RunHarness(string distributionDirectory)
    {
        var valheimDirectory = ValheimDirectory();
        var startInfo = new ProcessStartInfo
        {
            FileName = "/usr/bin/env",
            WorkingDirectory = valheimDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("bash");
        startInfo.ArgumentList.Add(Path.Combine(
            valheimDirectory,
            "tests/release-package-behavior.sh"));
        startInfo.ArgumentList.Add(Version);
        startInfo.ArgumentList.Add(distributionDirectory);

        using var process = Process.Start(startInfo)!;
        var standardOutput = process.StandardOutput.ReadToEnd();
        var standardError = process.StandardError.ReadToEnd();
        process.WaitForExit();
        return new ProcessResult(process.ExitCode, standardOutput, standardError);
    }

    private static string ReadValheimFile(string relativePath) =>
        File.ReadAllText(Path.Combine(ValheimDirectory(), relativePath));

    private static string ReadRepositoryFile(string relativePath) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(
            ValheimDirectory(),
            "../..",
            relativePath)));

    private static string ValheimDirectory() =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../"));

    private sealed record ProcessResult(
        int ExitCode,
        string StandardOutput,
        string StandardError);

    private sealed class TemporaryDirectory : IDisposable
    {
        public TemporaryDirectory()
        {
            Path = Directory.CreateTempSubdirectory("valheim-packages-").FullName;
        }

        public string Path { get; }

        public void Dispose()
        {
            if (Directory.Exists(Path))
            {
                Directory.Delete(Path, recursive: true);
            }
        }
    }
}
