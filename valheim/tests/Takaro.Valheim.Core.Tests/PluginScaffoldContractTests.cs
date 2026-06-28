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
    public void PluginBridgeIsServerSideOnly()
    {
        var pluginSource = ReadPluginSource("ValheimTakaroPlugin.cs");
        var bridgeSource = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(pluginSource, "connector is server-side only");
        Assert.IsFalse(bridgeSource.Contains("TakaroClient", StringComparison.Ordinal), "Valheim connector must not register client-side Takaro RPCs.");
        Assert.IsFalse(bridgeSource.Contains("ForwardLocal", StringComparison.Ordinal), "Valheim connector must not forward client-side events.");
    }

    [TestMethod]
    public void PluginBridgeHooksDeathEvents()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(source, "TakaroCharacterOnDeathPatch");
        StringAssert.Contains(source, "EventFactory.EntityKilled");
    }

    [TestMethod]
    public void PluginBridgeDoesNotForwardClientOwnedEntityDeathsToServer()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        Assert.IsFalse(source.Contains("RPC_TakaroEntityKilled", StringComparison.Ordinal), "Valheim connector must not rely on client-side entity kill forwarding.");
        Assert.IsFalse(source.Contains("ForwardLocalEntityKilled", StringComparison.Ordinal), "Valheim connector must not forward client-side entity kills.");
        StringAssert.Contains(source, "EmitEntityKilled");
    }

    [TestMethod]
    public void PluginBridgeDoesNotEmitMalformedChatPayloads()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");
        var emitMethod = SliceMethod(source, "public static void Emit(long senderId", "public static void EmitLog");

        StringAssert.Contains(emitMethod, "IsSafeChatText(text)");
        StringAssert.Contains(emitMethod, "TryResolveChatPlayer");
        StringAssert.Contains(source, "ContainsUnsafeChatIdentity(userInfo.Name)");
        StringAssert.Contains(source, "ContainsUnsafeChatIdentity(userInfo.GetDisplayName())");
        StringAssert.Contains(source, "Takaro Valheim dropped malformed chat event");
        StringAssert.Contains(source, "not emitting until payload layout is known");
        StringAssert.Contains(source, "LogUndecodedDedicatedChatPacket(data);");
        StringAssert.Contains(source, "Prefix(ZRoutedRpc.RoutedRPCData rpcData)");
        StringAssert.Contains(source, "ObserveRoutedRpcData(rpcData)");
        Assert.IsFalse(source.Contains("DedicatedServerSayLikeHashes.Contains(data.m_methodHash)"), "Unknown routed hashes must not be decoded as Say without payload proof.");
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
