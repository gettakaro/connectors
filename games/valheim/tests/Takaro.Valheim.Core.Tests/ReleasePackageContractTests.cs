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
    public void ValidSeparateServerAndClientFixturesPass()
    {
        using var fixture = CreateFixture();

        var result = RunHarness(fixture.Path);

        Assert.AreEqual(0, result.ExitCode, result.StandardError);
    }

    [DataTestMethod]
    [DataRow("missing-client-dll")]
    [DataRow("wrong-client-role")]
    [DataRow("server-dll-in-client")]
    [DataRow("client-dll-in-server")]
    [DataRow("core-dll-in-client")]
    [DataRow("config-in-client")]
    [DataRow("missing-server-config")]
    [DataRow("token-in-server-config")]
    [DataRow("identity-in-server-config")]
    [DataRow("second-config-in-server")]
    [DataRow("pdb-in-client")]
    [DataRow("deps-in-client")]
    [DataRow("host-dll-in-client")]
    [DataRow("jotunn-in-client")]
    [DataRow("cloud-marker-in-client")]
    [DataRow("product-version-mismatch")]
    [DataRow("protocol-version-mismatch")]
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
    public void ReleaseScriptsAndWorkflowPublishBothRoleSpecificArchives()
    {
        var harness = ReadValheimFile("tests/release-package-behavior.sh");
        var release = ReadValheimFile("scripts/build-release.sh");
        var workflow = ReadRepositoryFile(".github/workflows/valheim.yml");
        var game = ReadRepositoryFile("catalog/valheim/game.json");

        // The release script spells neither archive name: it takes both from the resolved
        // target, one key per role, so a re-pin renames the artifacts on its own.
        StringAssert.Contains(release, "VALHEIM_ARTIFACT_SERVER_PLUGIN");
        StringAssert.Contains(release, "VALHEIM_ARTIFACT_CLIENT_COMPANION");
        Assert.IsFalse(
            release.Contains("takaro-valheim-plugin", StringComparison.Ordinal),
            "the release script must not hard-code an archive name the catalog owns.");

        // The harness still finds one archive per role by name, whatever the target is.
        StringAssert.Contains(harness, "takaro-valheim-plugin");
        StringAssert.Contains(harness, "takaro-valheim-inventory-companion");

        // Both patterns and both legacy aliases are declared in one place: the game record.
        StringAssert.Contains(game, "takaro-valheim-plugin-{target}-{version}.zip");
        StringAssert.Contains(game, "takaro-valheim-inventory-companion-{target}-{version}.zip");
        StringAssert.Contains(game, "\"takaro-valheim-plugin.zip\"");
        StringAssert.Contains(game, "\"takaro-valheim-inventory-companion.zip\"");
        StringAssert.Contains(game, "server-plugin");
        StringAssert.Contains(game, "client-companion");

        StringAssert.Contains(harness, "rg -a -q");
        Assert.IsFalse(harness.Contains("rg -q \"$marker\" \"$client_zip\"", StringComparison.Ordinal));
        StringAssert.Contains(release, "SOURCE_DATE_EPOCH");
        StringAssert.Contains(release, "zip -X");
        StringAssert.Contains(release, "LC_ALL=C sort");
        StringAssert.Contains(release, "release-package-behavior.sh");

        // One publisher for every connector: the workflow hands both roles to the shared
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
            2,
            release.Split(cachePath, StringSplitOptions.None).Length - 1,
            "Both packaged role READMEs must invalidate BepInEx's metadata cache.");
        StringAssert.Contains(release, "before restarting");
    }

    private static TemporaryDirectory CreateFixture(string? mutation = null)
    {
        var fixture = new TemporaryDirectory();
        var server = Path.Combine(fixture.Path, "server", "TakaroValheim");
        var client = Path.Combine(fixture.Path, "client", "TakaroValheimInventoryCompanion");
        Directory.CreateDirectory(server);
        Directory.CreateDirectory(client);

        Write(Path.Combine(server, "TakaroValheim.dll"), "server fixture");
        Write(Path.Combine(server, "Takaro.Valheim.Core.dll"), "core fixture");
        Write(Path.Combine(server, "Takaro.Valheim.Companion.Protocol.dll"), "protocol fixture");
        Write(Path.Combine(server, "README.txt"), "server install fixture");
        WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server");
        Write(Path.Combine(server, "takaro.cfg"), "[Takaro]\nregistrationToken =\nidentityToken =\n");

        Write(Path.Combine(client, "Takaro.Valheim.Companion.dll"), "client fixture");
        Write(Path.Combine(client, "Takaro.Valheim.Companion.Protocol.dll"), "protocol fixture");
        Write(Path.Combine(client, "README.txt"), "client install fixture");
        WriteManifest(Path.Combine(client, "manifest.json"), "graphical-client");

        switch (mutation)
        {
            case null:
                break;
            case "missing-client-dll":
                File.Delete(Path.Combine(client, "Takaro.Valheim.Companion.dll"));
                break;
            case "wrong-client-role":
                WriteManifest(Path.Combine(client, "manifest.json"), "dedicated-server");
                break;
            case "client-dll-in-server":
                Write(Path.Combine(server, "Takaro.Valheim.Companion.dll"), "wrong role");
                break;
            case "server-dll-in-client":
                Write(Path.Combine(client, "TakaroValheim.dll"), "wrong role");
                break;
            case "core-dll-in-client":
                Write(Path.Combine(client, "Takaro.Valheim.Core.dll"), "wrong role");
                break;
            case "missing-server-config":
                File.Delete(Path.Combine(server, "takaro.cfg"));
                break;
            case "token-in-server-config":
                Write(Path.Combine(server, "takaro.cfg"), "[Takaro]\nregistrationToken = secret\nidentityToken =\n");
                break;
            case "identity-in-server-config":
                Write(Path.Combine(server, "takaro.cfg"), "[Takaro]\nregistrationToken =\nidentityToken = shared\n");
                break;
            case "second-config-in-server":
                Write(Path.Combine(server, "com.takaro.valheim.cfg"), "registrationToken=secret");
                break;
            case "config-in-client":
                Write(Path.Combine(client, "com.takaro.valheim.cfg"), "registrationToken=secret");
                break;
            case "pdb-in-client":
                Write(Path.Combine(client, "Takaro.Valheim.Companion.pdb"), "debug");
                break;
            case "deps-in-client":
                Write(Path.Combine(client, "Takaro.Valheim.Companion.deps.json"), "{}");
                break;
            case "host-dll-in-client":
                Write(Path.Combine(client, "BepInEx.dll"), "host");
                break;
            case "jotunn-in-client":
                Write(Path.Combine(client, "Jotunn.dll"), "host");
                break;
            case "cloud-marker-in-client":
                Write(Path.Combine(client, "Takaro.Valheim.Companion.dll"), "ClientWebSocket connect.takaro.io");
                break;
            case "product-version-mismatch":
                WriteManifest(
                    Path.Combine(client, "manifest.json"),
                    "graphical-client",
                    version: "2.0.1");
                break;
            case "protocol-version-mismatch":
                WriteManifest(
                    Path.Combine(client, "manifest.json"),
                    "graphical-client",
                    protocolCurrent: 4);
                break;
            // The loader version, pack version and plugin version are independent fields.
            case "bepinex-version-equals-plugin-version":
                WriteManifest(
                    Path.Combine(client, "manifest.json"),
                    "graphical-client",
                    loaderVersion: PluginVersion);
                break;
            case "pack-version-floating":
                WriteManifest(
                    Path.Combine(client, "manifest.json"),
                    "graphical-client",
                    packVersion: "latest");
                break;
            case "missing-plugin-version":
                WriteManifest(
                    Path.Combine(client, "manifest.json"),
                    "graphical-client",
                    pluginVersion: null);
                break;
            default:
                Assert.Fail($"Unknown fixture mutation {mutation}.");
                break;
        }

        ZipFile.CreateFromDirectory(
            Path.Combine(fixture.Path, "server"),
            Path.Combine(fixture.Path, "takaro-valheim-plugin.zip"),
            CompressionLevel.NoCompression,
            includeBaseDirectory: false);
        ZipFile.CreateFromDirectory(
            Path.Combine(fixture.Path, "client"),
            Path.Combine(fixture.Path, "takaro-valheim-inventory-companion.zip"),
            CompressionLevel.NoCompression,
            includeBaseDirectory: false);
        return fixture;
    }

    private static void WriteManifest(
        string path,
        string role,
        string version = Version,
        int protocolCurrent = 3,
        string? pluginVersion = PluginVersion,
        string packVersion = PackVersion,
        string loaderVersion = LoaderVersion)
    {
        var manifest = new Dictionary<string, object?>
        {
            ["name"] = role == "dedicated-server"
                ? "TakaroValheim"
                : "TakaroValheimInventoryCompanion",
            ["productVersion"] = version,
            ["bepInExPack"] = new
            {
                @namespace = "denikson",
                name = "BepInExPack_Valheim",
                version = packVersion
            },
            ["bepInExVersion"] = loaderVersion,
            ["processRole"] = role,
            ["protocol"] = new
            {
                minimum = 3,
                current = protocolCurrent,
                maximum = 3
            }
        };
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
