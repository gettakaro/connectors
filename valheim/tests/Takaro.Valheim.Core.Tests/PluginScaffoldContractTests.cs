using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class PluginScaffoldContractTests
{
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
    public void PluginBridgeUsesServerOnlyChatAndEntityHooks()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(source, "TakaroChatRpcChatMessagePatch");
        StringAssert.Contains(source, "TakaroTalkerRpcSayPatch");
        StringAssert.Contains(source, "TakaroRoutedRpcPatch");
        StringAssert.Contains(source, "TakaroCharacterOnDeathPatch");
        StringAssert.Contains(source, "EventFactory.EntityKilled");
    }

    [TestMethod]
    public void PluginBridgeDoesNotDeclareClientSideRpcContracts()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        Assert.IsFalse(source.Contains("TakaroClientLocationSnapshot", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroClientInventorySnapshot", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroPlayerDeath", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroEntityKilled", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("Player.m_localPlayer", StringComparison.Ordinal));
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
        StringAssert.Contains(source, "player_component_unavailable");
    }

    [TestMethod]
    public void PluginAdapterReturnsExplicitErrorsForUnavailableServerOnlyPlayerState()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var locationMethod = SliceMethod(source, "public Task<TakaroActionResult> GetPlayerLocationAsync", "public Task<TakaroActionResult> GetPlayerInventoryAsync");
        var inventoryMethod = SliceMethod(source, "public Task<TakaroActionResult> GetPlayerInventoryAsync", "public Task<TakaroActionResult> GiveItemAsync");

        StringAssert.Contains(locationMethod, "player_position_unavailable");
        Assert.IsFalse(locationMethod.Contains("new TakaroPosition(0, 0, 0", StringComparison.Ordinal));

        StringAssert.Contains(inventoryMethod, "player_component_unavailable");
        Assert.IsFalse(inventoryMethod.Contains("Array.Empty<object>()", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginDoesNotStartOnClientProcesses()
    {
        var source = ReadPluginSource("ValheimTakaroPlugin.cs");

        StringAssert.Contains(source, "only runs on dedicated Valheim servers");
        Assert.IsFalse(source.Contains("client bridge started", StringComparison.Ordinal));
        Assert.IsTrue(source.IndexOf("if (!IsDedicatedServerProcess())", StringComparison.Ordinal)
            < source.IndexOf("harmony = new Harmony", StringComparison.Ordinal));
    }

    private static string ReadPluginSource(string fileName)
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Plugin",
            fileName));

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
