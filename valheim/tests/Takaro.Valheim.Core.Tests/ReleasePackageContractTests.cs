using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ReleasePackageContractTests
{
    private const string Version = "2.0.0-rc.1+package";

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
    [DataRow("core-dll-in-client")]
    [DataRow("config-in-client")]
    [DataRow("pdb-in-client")]
    [DataRow("deps-in-client")]
    [DataRow("host-dll-in-client")]
    [DataRow("jotunn-in-client")]
    [DataRow("cloud-marker-in-client")]
    [DataRow("product-version-mismatch")]
    [DataRow("protocol-version-mismatch")]
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

        foreach (var source in new[] { harness, release, workflow })
        {
            StringAssert.Contains(source, "takaro-valheim-plugin.zip");
            StringAssert.Contains(source, "takaro-valheim-companion.zip");
        }

        StringAssert.Contains(harness, "rg -a -q");
        Assert.IsFalse(harness.Contains("rg -q \"$marker\" \"$client_zip\"", StringComparison.Ordinal));
        StringAssert.Contains(release, "SOURCE_DATE_EPOCH");
        StringAssert.Contains(release, "zip -X");
        StringAssert.Contains(release, "LC_ALL=C sort");
        StringAssert.Contains(workflow, "release-package-behavior.sh");
    }

    private static TemporaryDirectory CreateFixture(string? mutation = null)
    {
        var fixture = new TemporaryDirectory();
        var server = Path.Combine(fixture.Path, "server", "TakaroValheim");
        var client = Path.Combine(fixture.Path, "client", "TakaroValheimCompanion");
        Directory.CreateDirectory(server);
        Directory.CreateDirectory(client);

        Write(Path.Combine(server, "TakaroValheim.dll"), "server fixture");
        Write(Path.Combine(server, "Takaro.Valheim.Core.dll"), "core fixture");
        Write(Path.Combine(server, "Takaro.Valheim.Companion.Protocol.dll"), "protocol fixture");
        Write(Path.Combine(server, "README.txt"), "server install fixture");
        WriteManifest(Path.Combine(server, "manifest.json"), "dedicated-server");

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
            case "server-dll-in-client":
                Write(Path.Combine(client, "TakaroValheim.dll"), "wrong role");
                break;
            case "core-dll-in-client":
                Write(Path.Combine(client, "Takaro.Valheim.Core.dll"), "wrong role");
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
                    protocolCurrent: 2);
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
            Path.Combine(fixture.Path, "takaro-valheim-companion.zip"),
            CompressionLevel.NoCompression,
            includeBaseDirectory: false);
        return fixture;
    }

    private static void WriteManifest(
        string path,
        string role,
        string version = Version,
        int protocolCurrent = 1)
    {
        var manifest = new
        {
            name = role == "dedicated-server"
                ? "TakaroValheim"
                : "TakaroValheimCompanion",
            productVersion = version,
            bepInExVersion = "2.0.0",
            processRole = role,
            protocol = new
            {
                minimum = 1,
                current = protocolCurrent,
                maximum = 1
            }
        };
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
            "..",
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
