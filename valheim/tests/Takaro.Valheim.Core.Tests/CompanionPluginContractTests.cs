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

    [TestMethod]
    public void CompanionTargetsOnlyExactLiveServerPeer()
    {
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");

        StringAssert.Contains(bridge, "network.GetServerPeer()");
        StringAssert.Contains(bridge, "serverPeer.m_uid");
        StringAssert.Contains(
            bridge,
            "InvokeRoutedRPC(serverPeer.m_uid, CompanionProtocol.RpcName, json)");
        Assert.IsFalse(bridge.Contains(
            "InvokeRoutedRPC(CompanionProtocol.RpcName",
            StringComparison.Ordinal));
        Assert.IsFalse(bridge.Contains("GetServerPeerID()", StringComparison.Ordinal));
        Assert.IsFalse(bridge.Contains("Everybody", StringComparison.Ordinal));
        Assert.IsFalse(bridge.Contains("EverybodyExceptPeer", StringComparison.Ordinal));
    }

    [TestMethod]
    public void CompanionResetsOnRpcWorldServerAndDisconnectChanges()
    {
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");

        StringAssert.Contains(bridge, "ReferenceEquals(routedRpc, registeredRpc)");
        StringAssert.Contains(bridge, "registrationAttempted");
        StringAssert.Contains(bridge, "(sender, json) => HandleEnvelope(sourceRpc, sender, json)");
        StringAssert.Contains(bridge, "ZNet.World");
        StringAssert.Contains(bridge, "world.m_uid");
        StringAssert.Contains(bridge, "network.GetServerPeer()");
        StringAssert.Contains(bridge, "state.ResetConnection()");
        StringAssert.Contains(bridge, "IsConnectionReady");
        StringAssert.Contains(bridge, "sender != serverPeer.m_uid");
        StringAssert.Contains(bridge, "ReferenceEquals(ZRoutedRpc.instance, registeredRpc)");
        Assert.IsFalse(bridge.Contains("GetWorldUID()", StringComparison.Ordinal));
    }

    [TestMethod]
    public void CompanionPatchesOnlyLocalTalkerSayForChatReports()
    {
        var hooks = ReadCompanionSource("CompanionClientHooks.cs");
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");
        var entrypoint = ReadCompanionSource("ValheimCompanionPlugin.cs");

        StringAssert.Contains(hooks, "[HarmonyPatch(typeof(Talker), \"Say\")]");
        StringAssert.Contains(hooks, "__instance.GetComponent<Player>() == Player.m_localPlayer");
        StringAssert.Contains(hooks, "private static bool Prefix(");
        StringAssert.Contains(hooks, "out CompanionChatHookState __state");
        StringAssert.Contains(hooks, "private static void Postfix(CompanionChatHookState __state)");
        Assert.IsFalse(hooks.Contains("RPC_Say", StringComparison.Ordinal));
        StringAssert.Contains(bridge, "TrySendChat(string message)");
        StringAssert.Contains(entrypoint, "CompanionClientHooks.Initialize(");
        StringAssert.Contains(entrypoint, "CompanionClientHooks.Shutdown()");
        StringAssert.Contains(entrypoint, "companionCommandPrefixes");
    }

    [TestMethod]
    public void CompanionPollsBoundedInventoryOnlyAfterNegotiation()
    {
        var reader = ReadCompanionSource("CompanionInventoryReader.cs");
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");
        var hooks = ReadCompanionSource("CompanionClientHooks.cs");

        StringAssert.Contains(reader, "inventory.GetAllItems()");
        StringAssert.Contains(reader, "item.m_dropPrefab");
        StringAssert.Contains(reader, "item.m_gridPos");
        StringAssert.Contains(bridge, "InventoryPollInterval");
        StringAssert.Contains(bridge, "Player.m_localPlayer");
        StringAssert.Contains(bridge, "state.HasCapability(CompanionCapability.Inventory)");
        StringAssert.Contains(bridge, "inventoryReader.TryReadChanged(");
        StringAssert.Contains(bridge, "inventoryReader.MarkSent(snapshot)");
        StringAssert.Contains(bridge, "inventoryReader.Reset()");
        Assert.IsFalse(hooks.Contains("typeof(Player), \"Update\"", StringComparison.Ordinal));
        Assert.IsFalse(bridge.Contains("stack(s)", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void CompanionCombatReportsShareNegotiatedExactServerTransport()
    {
        var bridge = ReadCompanionSource("CompanionClientBridge.cs");

        StringAssert.Contains(bridge, "TrySendPlayerDeath(CompanionPlayerDeathReport report)");
        StringAssert.Contains(bridge, "TrySendEntityKilled(CompanionEntityKilledReport report)");
        StringAssert.Contains(bridge, "CompanionMessageTypes.PlayerDeath");
        StringAssert.Contains(bridge, "CompanionMessageTypes.EntityKilled");
        StringAssert.Contains(bridge, "TrySendReport(");
        Assert.IsFalse(bridge.Contains("InvokeRoutedRPC(CompanionMessageTypes", StringComparison.Ordinal));
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
