using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionPluginContractTests
{
    [TestMethod]
    public void CompanionLoadsOnlyInGraphicalValheimClient()
    {
        Assert.IsTrue(CompanionRuntimePolicy.IsGraphicalValheimClient(
            isBatchMode: false,
            processName: "valheim",
            executablePath: "/games/Valheim/valheim.x86_64"));
        Assert.IsFalse(CompanionRuntimePolicy.IsGraphicalValheimClient(
            isBatchMode: false,
            processName: "bash",
            executablePath: "/usr/bin/bash"));

        var entrypoint = ReadCompanionSource("ValheimCompanionPlugin.cs");
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");
        StringAssert.Contains(entrypoint, "com.takaro.valheim.companion");
        StringAssert.Contains(entrypoint, "[BepInPlugin(PluginGuid, PluginName, PluginVersion)]");
        StringAssert.Contains(entrypoint, "TakaroCompanionBuildVersion.ProductVersion");
        StringAssert.Contains(entrypoint, "TakaroCompanionBuildVersion.ProtocolVersion");
        StringAssert.Contains(entrypoint, "harmony.PatchAll(typeof(ValheimCompanionPlugin).Assembly)");
        StringAssert.Contains(entrypoint, "clientBridge.Initialize()");
        StringAssert.Contains(entrypoint, "clientBridge?.Update()");
        StringAssert.Contains(entrypoint, "clientBridge?.Dispose()");
        StringAssert.Contains(bridge, "sealed class CompanionClientBridge");
    }

    [TestMethod]
    public void CompanionRefusesDedicatedServerProcess()
    {
        Assert.IsFalse(CompanionRuntimePolicy.IsGraphicalValheimClient(
            isBatchMode: true,
            processName: "valheim",
            executablePath: "C:/games/Valheim/valheim.exe"));
        Assert.IsFalse(CompanionRuntimePolicy.IsGraphicalValheimClient(
            isBatchMode: false,
            processName: "valheim_server",
            executablePath: "/servers/valheim_server.x86_64"));

        var entrypoint = ReadCompanionSource("ValheimCompanionPlugin.cs");
        var guardAt = entrypoint.IndexOf("if (!IsGraphicalValheimClient())", StringComparison.Ordinal);
        var patchAt = entrypoint.IndexOf("harmony = new Harmony", StringComparison.Ordinal);
        Assert.IsTrue(guardAt >= 0 && guardAt < patchAt);
        StringAssert.Contains(entrypoint, "enabled = false");
    }

    [TestMethod]
    public void CompanionReferencesProtocolButNeverCore()
    {
        var project = ReadCompanionFile("Takaro.Valheim.Companion.csproj");
        var solution = ReadValheimFile("Takaro.Valheim.sln");

        StringAssert.Contains(project, "<TargetFrameworks>net8.0;net472</TargetFrameworks>");
        StringAssert.Contains(project, "<AssemblyName>Takaro.Valheim.Companion</AssemblyName>");
        StringAssert.Contains(project, "EnableValheimCompanionBuild");
        StringAssert.Contains(project, "Takaro.Valheim.Companion.Protocol.csproj");
        Assert.IsFalse(project.Contains("Takaro.Valheim.Core.csproj", StringComparison.Ordinal));
        StringAssert.Contains(solution, "Takaro.Valheim.Companion.csproj");
    }

    [TestMethod]
    public void CompanionContainsNoTakaroCredentialsOrCloudTransport()
    {
        var combined = ReadAllCompanionText();
        foreach (var forbidden in new[]
                 {
                     "registrationToken",
                     "identityToken",
                     "takaroWsUrl",
                     "TakaroWebSocketRunner",
                     "ClientWebSocket",
                     "HttpClient",
                     "connect.takaro.io",
                     "wss://",
                     "https://"
                 })
        {
            Assert.IsFalse(
                combined.Contains(forbidden, StringComparison.OrdinalIgnoreCase),
                forbidden);
        }
    }

    [TestMethod]
    public void CompanionUsesNoJotunnOrServerSync()
    {
        var combined = ReadAllCompanionText();
        foreach (var forbidden in new[] { "Jotunn", "ServerSync", "BepInDependency" })
        {
            Assert.IsFalse(
                combined.Contains(forbidden, StringComparison.OrdinalIgnoreCase),
                forbidden);
        }
    }

    [TestMethod]
    public void ServerPluginStillRefusesGraphicalClient()
    {
        var serverEntrypoint = ReadValheimFile(
            "src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs");
        var guardAt = serverEntrypoint.IndexOf(
            "if (!IsDedicatedServerProcess())",
            StringComparison.Ordinal);
        var patchAt = serverEntrypoint.IndexOf(
            "harmony = new Harmony",
            StringComparison.Ordinal);

        Assert.IsTrue(guardAt >= 0 && guardAt < patchAt);
        StringAssert.Contains(
            serverEntrypoint,
            "only runs on dedicated Valheim servers");
    }

    private static string ReadAllCompanionText() =>
        string.Join(
            '\n',
            Directory.GetFiles(CompanionPath(), "*", SearchOption.AllDirectories)
                .Where(path => path.EndsWith(".cs", StringComparison.Ordinal)
                    || path.EndsWith(".csproj", StringComparison.Ordinal))
                .Where(path => !path.Contains(
                    $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal)
                    && !path.Contains(
                        $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal))
                .OrderBy(path => path, StringComparer.Ordinal)
                .Select(File.ReadAllText));

    private static string ReadCompanionSource(string fileName) =>
        File.ReadAllText(Path.Combine(CompanionPath(), fileName));

    private static string ReadCompanionFile(string fileName) =>
        File.ReadAllText(Path.Combine(CompanionPath(), fileName));

    private static string ReadValheimFile(string relativePath) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../",
            relativePath)));

    private static string CompanionPath() =>
        Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Companion"));
}
