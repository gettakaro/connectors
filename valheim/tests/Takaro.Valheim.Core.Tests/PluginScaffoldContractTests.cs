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
    public async Task ReferenceFreeScaffoldDoesNotFabricateEmptyRuntimeArrays()
    {
        var dispatcher = new TakaroRequestDispatcher(new ValheimServerAdapter());

        var bansResult = await dispatcher.DispatchAsync(new TakaroRequest("list-bans", "listBans", JsonDocument.Parse("""[]""").RootElement));
        Assert.IsFalse(bansResult.Success);
        Assert.AreEqual("runtime_unavailable", bansResult.ErrorCode);

        var locationsResult = await dispatcher.DispatchAsync(new TakaroRequest("list-locations", "listLocations", JsonDocument.Parse("""[]""").RootElement));
        Assert.IsFalse(locationsResult.Success);
        Assert.AreEqual("runtime_unavailable", locationsResult.ErrorCode);
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
    public void PluginAdapterUsesCompanionInventoryWithoutPlayerComponentAccess()
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
        StringAssert.Contains(inventory, "CompanionMode.Disabled");
        StringAssert.Contains(inventory, "TryResolvePlayer(identifier");
        StringAssert.Contains(inventory, "CompanionInventoryActionPolicy.FromResolvedPlayer");
        Assert.IsFalse(inventory.Contains("PlayerMapper.Find", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("GetPlayerList()", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("Array.Empty<object>()", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("GetInventory()", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("TryFindPlayerComponent", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("companionInventory.TryGet(identifier", StringComparison.Ordinal));

        var policy = ReadValheimFile("src/Takaro.Valheim.Core/CompanionInventoryActionPolicy.cs");
        StringAssert.Contains(policy, "cache.TryGetStable");
        Assert.IsFalse(policy.Contains("player.Name", StringComparison.Ordinal));
        Assert.IsFalse(policy.Contains("cache.TryGet(alias", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginPlayerResolverOwnsAuthoritativeIdentityResolution()
    {
        var resolverPath = ValheimPath("src/Takaro.Valheim.Plugin/ValheimPlayerResolver.cs");
        Assert.IsTrue(File.Exists(resolverPath), "Missing authoritative Valheim player resolver.");

        var resolver = File.ReadAllText(resolverPath);
        var adapter = ReadPluginSource("ValheimServerAdapter.cs");

        StringAssert.Contains(resolver, "public sealed class ValheimPlayerResolver");
        StringAssert.Contains(resolver, "TakaroPlayer ToTakaroPlayer(ZNet.PlayerInfo");
        StringAssert.Contains(resolver, "TakaroPlayer ToTakaroPlayer(ZNetPeer");
        StringAssert.Contains(resolver, "bool TryResolvePlayer(");
        StringAssert.Contains(resolver, "bool TryFindPlayerInfo(");
        StringAssert.Contains(resolver, "bool TryFindPeer(");
        StringAssert.Contains(resolver, "PlayerMapper.TryFindUnique");
        StringAssert.Contains(resolver, "out var playerInfoAmbiguous");
        StringAssert.Contains(resolver, "if (playerInfoAmbiguous)");
        StringAssert.Contains(resolver, "GetPlayerList()");
        StringAssert.Contains(resolver, "GetPeers()");
        StringAssert.Contains(resolver, "peerCandidates.Select");
        Assert.IsFalse(resolver.Contains("PlayerMapper.Find(", StringComparison.Ordinal));

        StringAssert.Contains(adapter, "private readonly ValheimPlayerResolver playerResolver;");
        StringAssert.Contains(adapter, "playerResolver.ToTakaroPlayer");
        StringAssert.Contains(adapter, "playerResolver.TryResolvePlayer");
        Assert.IsFalse(adapter.Contains("private TakaroPlayer ToTakaroPlayer(", StringComparison.Ordinal));
        Assert.IsFalse(adapter.Contains("private bool TryResolvePlayer(", StringComparison.Ordinal));
        Assert.IsFalse(adapter.Contains("private bool TryFindPlayerInfo(", StringComparison.Ordinal));
        Assert.IsFalse(adapter.Contains("private bool TryFindPeer(", StringComparison.Ordinal));
        Assert.IsFalse(adapter.Contains("PlayerMapper.TryFindUnique", StringComparison.Ordinal));
        Assert.IsFalse(adapter.Contains("m_userInfo", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginPlayerResolverResolvesExactConnectedPeerUidWithoutPayloadIdentity()
    {
        var resolverPath = ValheimPath("src/Takaro.Valheim.Plugin/ValheimPlayerResolver.cs");
        Assert.IsTrue(File.Exists(resolverPath), "Missing authoritative Valheim player resolver.");

        var resolver = File.ReadAllText(resolverPath);

        StringAssert.Contains(resolver, "public bool TryResolveConnectedPeer(");
        StringAssert.Contains(resolver, "long sender");
        StringAssert.Contains(resolver, "out ZNetPeer? peer");
        StringAssert.Contains(resolver, "out TakaroPlayer? player");
        StringAssert.Contains(resolver, "PeerResolutionPolicy.TryResolveReadySender(");
        StringAssert.Contains(resolver, "candidate.IsReady()");
        Assert.IsFalse(resolver.Contains("payload", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void PluginCompanionBridgeKeepsTrustedReportsSeparateFromRoutedDiagnostics()
    {
        var bridge = ReadPluginSource("CompanionServerBridge.cs");
        var diagnostics = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(bridge, "CompanionServerMessageHandler");
        StringAssert.Contains(bridge, "TryResolveConnectedPeer(sender");
        StringAssert.Contains(bridge, "CompanionAcceptedEvent acceptedEvent");
        StringAssert.Contains(diagnostics, "untrusted routed RPC diagnostics");
        StringAssert.Contains(diagnostics, "observation only");
        Assert.IsFalse(diagnostics.Contains("CompanionProtocol.RpcName", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginWiresOneSharedCompanionGraphOnTheMainThread()
    {
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");

        StringAssert.Contains(entrypoint, "[\"companionMode\"]");
        StringAssert.Contains(entrypoint, "[\"companionCommandPrefixes\"]");
        StringAssert.Contains(entrypoint, "private CompanionInventoryCache? companionInventory;");
        StringAssert.Contains(entrypoint, "private CompanionServerBridge? companionBridge;");
        StringAssert.Contains(entrypoint, "new ValheimPlayerResolver(Logger)");
        StringAssert.Contains(entrypoint, "companionInventory,");
        StringAssert.Contains(entrypoint, "playerResolver);");
        StringAssert.Contains(entrypoint, "config.CompanionMode");
        StringAssert.Contains(entrypoint, "config.CompanionMode == CompanionMode.Disabled");

        var cacheAt = entrypoint.IndexOf("companionInventory = new CompanionInventoryCache()", StringComparison.Ordinal);
        var resolverAt = entrypoint.IndexOf("new ValheimPlayerResolver(Logger)", StringComparison.Ordinal);
        var adapterAt = entrypoint.IndexOf("new ValheimServerAdapter(", StringComparison.Ordinal);
        var runnerAt = entrypoint.IndexOf("new TakaroWebSocketRunner(", StringComparison.Ordinal);
        var bridgeAt = entrypoint.IndexOf("new CompanionServerBridge(", StringComparison.Ordinal);
        Assert.IsTrue(cacheAt >= 0 && cacheAt < resolverAt);
        Assert.IsTrue(resolverAt < adapterAt);
        Assert.IsTrue(adapterAt < runnerAt);
        Assert.IsTrue(runnerAt < bridgeAt);

        var update = SliceMethod(entrypoint, "private void Update()", "private void OnDestroy()");
        Assert.IsTrue(
            update.IndexOf("mainThreadActions?.Drain()", StringComparison.Ordinal)
            < update.IndexOf("companionBridge?.Update()", StringComparison.Ordinal));

        var destroy = SliceMethod(entrypoint, "private void OnDestroy()", "private void RequestShutdown()");
        Assert.IsTrue(
            destroy.IndexOf("companionBridge?.Dispose()", StringComparison.Ordinal)
            < destroy.IndexOf("runner?.Dispose()", StringComparison.Ordinal));
        Assert.IsTrue(
            destroy.IndexOf("runner?.Dispose()", StringComparison.Ordinal)
            < destroy.IndexOf("mainThreadActions?.Dispose()", StringComparison.Ordinal));
    }

    [TestMethod]
    public void RealPluginAdapterExposesCompanionInventoryCacheInjection()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var realAdapter = source[..source.IndexOf("#else", StringComparison.Ordinal)];

        StringAssert.Contains(realAdapter, "private readonly CompanionInventoryCache companionInventory;");
        StringAssert.Contains(realAdapter, "CompanionInventoryCache companionInventory");
        StringAssert.Contains(realAdapter, "this.companionInventory = companionInventory");
        StringAssert.Contains(realAdapter, "private readonly ValheimPlayerResolver playerResolver;");
        StringAssert.Contains(realAdapter, "new ValheimPlayerResolver(logger)");
        StringAssert.Contains(realAdapter, "ValheimPlayerResolver playerResolver");
        StringAssert.Contains(realAdapter, "this.playerResolver = playerResolver");
    }

    [TestMethod]
    public void RunnerGatesLifecycleAndWireResponsesOnHonestServerState()
    {
        var source = ReadPluginSource("TakaroWebSocketRunner.cs");

        StringAssert.Contains(source, "GetPlayerLocationAsync(player.GameId");
        StringAssert.Contains(source, "TryCreateActionResponse");
        StringAssert.Contains(source, "SuppressedResponseLogLimiter");
        Assert.IsTrue(
            source.IndexOf("GetPlayerLocationAsync(player.GameId", StringComparison.Ordinal)
                < source.IndexOf("lifecycleCoordinator.Update", StringComparison.Ordinal),
            "Lifecycle tracking must see only players with real server-owned positions.");
    }

    [TestMethod]
    public void PluginDrainsAndDisposesTheBoundedMainThreadScheduler()
    {
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");
        var runner = ReadPluginSource("TakaroWebSocketRunner.cs");
        var scheduler = ReadValheimFile("src/Takaro.Valheim.Core/MainThreadActionScheduler.cs");

        StringAssert.Contains(entrypoint, "new QueuedMainThreadActionScheduler");
        StringAssert.Contains(entrypoint, "mainThreadActions?.Drain()");
        StringAssert.Contains(entrypoint, "mainThreadActions?.Dispose()");
        StringAssert.Contains(runner, "new TakaroRequestDispatcher(adapter, this.mainThreadActions)");
        StringAssert.Contains(runner, "mainThreadActions.ScheduleAsync");
        StringAssert.Contains(scheduler, "TaskCreationOptions.RunContinuationsAsynchronously");
        StringAssert.Contains(scheduler, "capacity");
    }

    [TestMethod]
    public void ShutdownAndGameEventIoDoNotCallUnityOrWebSocketsFromTheWrongThread()
    {
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");
        var adapter = ReadPluginSource("ValheimServerAdapter.cs");
        var runner = ReadPluginSource("TakaroWebSocketRunner.cs");
        var shutdown = SliceMethod(
            adapter,
            "public Task<TakaroActionResult> ShutdownAsync",
            "private static void SendHudMessage");

        StringAssert.Contains(shutdown, "requestShutdown()");
        Assert.IsFalse(shutdown.Contains("Task.Run", StringComparison.Ordinal));
        Assert.IsFalse(shutdown.Contains("Application.Quit", StringComparison.Ordinal));
        StringAssert.Contains(entrypoint, "Application.Quit()");
        StringAssert.Contains(entrypoint, "shutdownRequestedAt");
        StringAssert.Contains(runner, "Task.Run(() => SendGameEventCoreAsync");
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

        StringAssert.Contains(give, "DropItemStack");
        StringAssert.Contains(source, "ItemDrop.DropItem");
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

        StringAssert.Contains(give, "GiveItemPolicy.PlanStacks");
        StringAssert.Contains(give, "amountValidation.ErrorCode");
        StringAssert.Contains(give, "stackPlan.ErrorCode");
        StringAssert.Contains(give, "invalid_quality");
        StringAssert.Contains(give, "position_unavailable");
    }

    [TestMethod]
    public void GiveItemConfirmationCannotBeReemittedAsInboundChat()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var give = SliceMethod(
            source,
            "public Task<TakaroActionResult> GiveItemAsync",
            "public Task<TakaroActionResult> SendMessageAsync");

        StringAssert.Contains(give, "SendHudMessage");
        Assert.IsFalse(give.Contains("SendChatMessage", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginBridgeDoesNotEmitIdentityEventsFromRoutedPayloads()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(source, "OnDeathHash");
        StringAssert.Contains(source, "data.m_methodHash == OnDeathHash");
        StringAssert.Contains(source, "ValheimEventAcceptancePolicy");
        Assert.IsFalse(source.Contains("EmitPlayerDeathFromRoutedRpc", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("EventFactory.ChatMessage", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"chat-message\"", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"player-death\"", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"TakaroPlayerDeath\"", StringComparison.Ordinal));
    }

    [TestMethod]
    public void DestroyZdoIsNotTreatedAsAChatDiagnosticCandidate()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        Assert.IsFalse(source.Contains("199378019", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("UndecodedDedicatedServerChatHashes", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("dedicated chat candidate", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void DestroyZdoHousekeepingDoesNotConsumeGenericRpcDiagnostics()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        StringAssert.Contains(source, "DestroyZdoHash = \"DestroyZDO\".GetStableHashCode()");
        var ignore = source.IndexOf("data.m_methodHash == DestroyZdoHash", StringComparison.Ordinal);
        var genericDiagnostic = source.IndexOf("if (routedDiagnosticsRemaining > 0)", StringComparison.Ordinal);
        Assert.IsTrue(ignore >= 0 && ignore < genericDiagnostic);
    }

    [TestMethod]
    public void UnsupportedEntityKilledEventHasNoPluginEmitterOrHarmonyPatch()
    {
        var source = ReadPluginSource("ValheimChatEventBridge.cs");

        Assert.IsFalse(source.Contains("EmitEntityKilled", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroCharacterOnDeathPatch", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("entity-killed event sent", StringComparison.Ordinal));
    }

    [TestMethod]
    public void LifecycleTransportLogsFrameWriteWithoutClaimingPersistence()
    {
        var source = ReadPluginSource("TakaroWebSocketRunner.cs");

        StringAssert.Contains(source, "lifecycle frame written");
        Assert.IsFalse(source.Contains("event sent for", StringComparison.Ordinal));
        StringAssert.Contains(source, "TakaroProtocol.TryCreateActionResponse");
    }

    [TestMethod]
    public void ObsoleteClientSnapshotCachesAndTestsAreRemoved()
    {
        var inventory = ReadValheimFile("src/Takaro.Valheim.Core/Inventory.cs");
        Assert.IsFalse(inventory.Contains("LocationSnapshotCache", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("InventorySnapshotCache", StringComparison.Ordinal));
        Assert.IsFalse(File.Exists(ValheimPath("tests/Takaro.Valheim.Core.Tests/LocationSnapshotCacheTests.cs")));
        Assert.IsFalse(File.Exists(ValheimPath("tests/Takaro.Valheim.Core.Tests/InventorySnapshotCacheTests.cs")));
    }

    [TestMethod]
    public void PluginUsesCoreRuntimeAndWorldDropPolicies()
    {
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");
        var adapter = ReadPluginSource("ValheimServerAdapter.cs");

        StringAssert.Contains(entrypoint, "ValheimRuntimePolicy.IsDedicatedServerProcess");
        StringAssert.Contains(adapter, "GiveItemPolicy.PlanStacks");
        StringAssert.Contains(adapter, "RuntimeArrayActionPolicy");
        StringAssert.Contains(adapter, "playerPositions.SwitchWorld");
        foreach (var method in new[]
                 {
                     SliceMethod(adapter, "public Task<TakaroActionResult> GetPlayersAsync", "public async Task<TakaroActionResult> GetPlayerAsync"),
                     SliceMethod(adapter, "public Task<TakaroActionResult> ListItemsAsync", "public Task<TakaroActionResult> ListEntitiesAsync"),
                     SliceMethod(adapter, "public Task<TakaroActionResult> ListEntitiesAsync", "public Task<TakaroActionResult> ListLocationsAsync"),
                     SliceMethod(adapter, "public Task<TakaroActionResult> ListLocationsAsync", "public Task<TakaroActionResult> GetMapInfoAsync"),
                     SliceMethod(adapter, "public Task<TakaroActionResult> ListBansAsync", "public Task<TakaroActionResult> ShutdownAsync")
                 })
        {
            Assert.IsFalse(method.Contains("?? []", StringComparison.Ordinal));
            StringAssert.Contains(method, "RuntimeArrayActionPolicy");
        }
    }

    [TestMethod]
    public void SourceAndPackagedInstallFlowsRequireRestartAfterConfiguration()
    {
        var readme = ReadValheimFile("README.md");
        var release = ReadValheimFile("scripts/build-release.sh");

        StringAssert.Contains(readme, "Restart the dedicated server");
        StringAssert.Contains(release, "Restart the dedicated server");
    }

    [TestMethod]
    public void ServerOnlyPluginHasNoJotunnDependencyAndRetriesReferenceSetup()
    {
        var project = ReadValheimFile("src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");
        var setup = ReadValheimFile("scripts/setup-environment.sh");
        var release = ReadValheimFile("scripts/build-release.sh");
        var combined = string.Join('\n', project, entrypoint, setup);

        foreach (var marker in new[] { "Jotunn", "JOTUNN_REFERENCE_PATH", "BepInDependency" })
        {
            Assert.IsFalse(combined.Contains(marker, StringComparison.OrdinalIgnoreCase), marker);
        }

        StringAssert.Contains(release, "rm -f");
        StringAssert.Contains(release, "Jotunn.dll");

        StringAssert.Contains(setup, "VALHEIM_STEAM_PLATFORMS");
        StringAssert.Contains(setup, "VALHEIM_REFERENCE_CACHE_DIR");
        StringAssert.Contains(setup, ".takaro-valheim-reference-cache");
        StringAssert.Contains(setup, "refusing to mutate");
        StringAssert.Contains(setup, "linux windows");
        StringAssert.Contains(setup, "MAX_ATTEMPTS");
        StringAssert.Contains(setup, "valheim_server_Data/Managed");
        StringAssert.Contains(setup, "appcache");
        StringAssert.Contains(setup, "--retry 5");
        StringAssert.Contains(setup, "--retry-delay 2");
        StringAssert.Contains(setup, "--retry-all-errors");
        StringAssert.Contains(setup, "command -v file");
        StringAssert.Contains(setup, "requires the 'file' command");
        StringAssert.Contains(setup, "Mono/.Net\\ assembly");
    }

    [TestMethod]
    public void WorkflowCachesTheOwnedReferenceDirectoryIncludingItsMarker()
    {
        var workflow = File.ReadAllText(Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../../.github/workflows/valheim.yml")));

        StringAssert.Contains(workflow, "valheim/_data/server\n");
        Assert.IsFalse(
            workflow.Contains("valheim/_data/server/valheim_server_Data/Managed", StringComparison.Ordinal),
            "Caching only Managed would drop the ownership marker and make a corrupt restored cache unrepairable.");
        StringAssert.Contains(workflow, "valheim-build-deps-v2-owned-reference-cache");
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

    private static string ValheimPath(string relativePath) =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../", relativePath));

    private static string SliceMethod(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        var end = source.IndexOf(endMarker, StringComparison.Ordinal);
        Assert.IsTrue(start >= 0, $"Missing source marker: {startMarker}");
        Assert.IsTrue(end > start, $"Missing source marker: {endMarker}");
        return source[start..end];
    }
}
