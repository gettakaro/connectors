using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PluginScaffoldContractTests
{
    [TestMethod]
    public async Task ReferenceFreeScaffoldReturnsExplicitUnavailablePlayerState()
    {
        var adapter = new ValheimServerAdapter();

        var location = await adapter.GetPlayerLocationAsync("Steam_1");
        var inventory = await adapter.GetPlayerInventoryAsync("Steam_1");

        Assert.IsFalse(location.Success);
        Assert.AreEqual("player_position_unavailable", location.ErrorCode);
        Assert.IsFalse(inventory.Success);
        Assert.AreEqual("player_component_unavailable", inventory.ErrorCode);
    }

    [TestMethod]
    public async Task ListOnlyActionsReturnBareArraysFromPluginAdapter()
    {
        var dispatcher = new TakaroRequestDispatcher(new ValheimServerAdapter());

        var bansResult = await dispatcher.DispatchAsync(new TakaroRequest("list-bans", "listBans", JsonDocument.Parse("""[]""").RootElement));
        using var bansDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("list-bans", bansResult));
        Assert.AreEqual(JsonValueKind.Array, bansDocument.RootElement.GetProperty("payload").ValueKind);

        var locationsResult = await dispatcher.DispatchAsync(new TakaroRequest("list-locations", "listLocations", JsonDocument.Parse("""[]""").RootElement));
        using var locationsDocument = JsonDocument.Parse(TakaroProtocol.CreateResponse("list-locations", locationsResult));
        Assert.AreEqual(JsonValueKind.Array, locationsDocument.RootElement.GetProperty("payload").ValueKind);
    }

    [TestMethod]
    public void BanPlayerDoesNotDirectlyDisconnectPeerAfterBan()
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs"));
        var source = File.ReadAllText(sourcePath);
        var banMethodStart = source.IndexOf("public Task<TakaroActionResult> BanPlayerAsync", StringComparison.Ordinal);
        var unbanMethodStart = source.IndexOf("public Task<TakaroActionResult> UnbanPlayerAsync", StringComparison.Ordinal);
        var banMethod = source[banMethodStart..unbanMethodStart];

        StringAssert.Contains(banMethod, "znet.Ban(primaryIdentifier);");
        Assert.IsFalse(banMethod.Contains("znet.Disconnect(peer)", StringComparison.Ordinal));
    }

    [TestMethod]
    public void KickPlayerDoesNotDirectlyDisconnectPeerAfterKickedRpc()
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs"));
        var source = File.ReadAllText(sourcePath);
        var kickMethodStart = source.IndexOf("public Task<TakaroActionResult> KickPlayerAsync", StringComparison.Ordinal);
        var banMethodStart = source.IndexOf("public Task<TakaroActionResult> BanPlayerAsync", StringComparison.Ordinal);
        var kickMethod = source[kickMethodStart..banMethodStart];

        StringAssert.Contains(kickMethod, """peer.m_rpc?.Invoke("Kicked");""");
        Assert.IsFalse(kickMethod.Contains("znet.Disconnect(peer)", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginAdapterUsesAllowlistedConsoleCommandExecution()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var method = SliceMethod(source, "public Task<TakaroActionResult> ExecuteConsoleCommandAsync", "public Task<TakaroActionResult> ListItemsAsync");

        StringAssert.Contains(method, "command_not_allowed");
        StringAssert.Contains(method, "success = false");
        StringAssert.Contains(method, "rawResult");
        StringAssert.Contains(method, "Console.instance.TryRunCommand(command, silentFail: false, skipAllowedCheck: true)");
        StringAssert.Contains(method, "ZNet.instance.RemoteCommand(command)");
    }

    [TestMethod]
    public void PluginAdapterListsNamedWorldLocations()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var method = SliceMethod(source, "public Task<TakaroActionResult> ListLocationsAsync", "public Task<TakaroActionResult> TeleportPlayerAsync");

        StringAssert.Contains(method, "GetLocationList()");
        StringAssert.Contains(method, "LocationFactory.Create");
        StringAssert.Contains(method, "m_location.m_name");
        StringAssert.Contains(method, "m_position");
    }

    [TestMethod]
    public void PluginDoesNotStartOnClientProcesses()
    {
        var source = ReadPluginSource("ValheimTakaroPlugin.cs");

        StringAssert.Contains(source, "if (!IsDedicatedServerProcess())");
        StringAssert.Contains(source, "only runs on dedicated Valheim servers");
        Assert.IsTrue(
            source.IndexOf("if (!IsDedicatedServerProcess())", StringComparison.Ordinal)
                < source.IndexOf("harmony = new Harmony", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("client bridge started", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void PluginBridgeDoesNotDeclareClientSideRpcContracts()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        foreach (var marker in new[]
                 {
                     "TakaroClientChatMessage",
                     "TakaroClientInventorySnapshot",
                     "TakaroClientLocationSnapshot",
                     "TakaroClientChatCommand",
                     "TakaroGiveItem",
                     "TakaroTeleportPlayer",
                     "TakaroPlayerDeath",
                     "TakaroEntityKilled",
                     "Player.m_localPlayer"
                 })
        {
            Assert.IsFalse(source.Contains(marker, StringComparison.Ordinal), marker);
        }
    }

    [TestMethod]
    public void PluginAdapterDoesNotRouteActionsThroughCustomClientRpc()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");

        Assert.IsFalse(source.Contains("TakaroGiveItem", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroTeleportPlayer", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroServerMessage", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TryGetLocationSnapshot", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TryGetInventorySnapshot", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginAdapterReturnsExplicitUnavailableErrorsForClientOwnedState()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var location = SliceMethod(
            source,
            "public Task<TakaroActionResult> GetPlayerLocationAsync",
            "public Task<TakaroActionResult> GetPlayerInventoryAsync");
        var inventory = SliceMethod(
            source,
            "public Task<TakaroActionResult> GetPlayerInventoryAsync",
            "public Task<TakaroActionResult> GiveItemAsync");

        StringAssert.Contains(location, "player_position_unavailable");
        Assert.IsFalse(location.Contains("new TakaroPosition(0, 0, 0", StringComparison.Ordinal));
        StringAssert.Contains(inventory, "player_component_unavailable");
        Assert.IsFalse(inventory.Contains("Array.Empty<object>()", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginAdapterUsesServerOwnedGiveAndTeleportPaths()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var give = SliceMethod(
            source,
            "public Task<TakaroActionResult> GiveItemAsync",
            "public Task<TakaroActionResult> SendMessageAsync");
        var teleport = SliceMethod(
            source,
            "public Task<TakaroActionResult> TeleportPlayerAsync",
            "public Task<TakaroActionResult> KickPlayerAsync");

        StringAssert.Contains(give, "ItemDrop");
        StringAssert.Contains(give, "Instantiate");
        StringAssert.Contains(teleport, "RPC_TeleportTo");
        Assert.IsFalse(give.Contains("TakaroGiveItem", StringComparison.Ordinal));
        Assert.IsFalse(teleport.Contains("TakaroTeleportPlayer", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginAdapterValidatesServerOwnedGiveRequests()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var give = SliceMethod(
            source,
            "public Task<TakaroActionResult> GiveItemAsync",
            "public Task<TakaroActionResult> SendMessageAsync");

        StringAssert.Contains(give, "amount <= 0");
        StringAssert.Contains(give, "invalid_amount");
        StringAssert.Contains(give, "invalid_quality");
        StringAssert.Contains(give, "position_unavailable");
    }

    [TestMethod]
    public void ServerOnlyPluginHasNoJotunnDependencyAndRetriesReferenceSetup()
    {
        var project = ReadValheimFile("src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");
        var setup = ReadValheimFile("scripts/setup-environment.sh");
        var release = ReadValheimFile("scripts/build-release.sh");
        var combined = string.Join('\n', project, entrypoint, setup, release);

        foreach (var marker in new[] { "Jotunn", "JOTUNN_REFERENCE_PATH", "BepInDependency" })
        {
            Assert.IsFalse(combined.Contains(marker, StringComparison.OrdinalIgnoreCase), marker);
        }

        StringAssert.Contains(setup, "VALHEIM_STEAM_PLATFORMS");
        StringAssert.Contains(setup, "linux windows");
        StringAssert.Contains(setup, "MAX_ATTEMPTS");
        StringAssert.Contains(setup, "valheim_server_Data/Managed");
        StringAssert.Contains(setup, "appcache");
        StringAssert.Contains(setup, "--retry 5");
        StringAssert.Contains(setup, "--retry-delay 2");
        StringAssert.Contains(setup, "--retry-all-errors");
    }

    private static string ReadPluginSource(string fileName)
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Plugin",
            fileName));

        return File.ReadAllText(sourcePath);
    }

    private static string ReadValheimFile(string relativePath)
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../",
            relativePath));

        return File.ReadAllText(sourcePath);
    }

    private static string SliceMethod(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        var end = source.IndexOf(endMarker, StringComparison.Ordinal);
        Assert.IsTrue(start >= 0, $"Missing source marker: {startMarker}");
        Assert.IsTrue(end > start, $"Missing source marker: {endMarker}");
        return source[start..end];
    }
}
