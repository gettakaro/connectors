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
            "../../../../../mod/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs"));
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
            "../../../../../mod/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs"));
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
        var source = string.Join(
            '\n',
            ReadPluginSource("ValheimServerEventBridge.cs"),
            ReadPluginSource("TakaroChatParticipant.cs"));

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
                     "Player.m_localPlayer",
                     ".Register(",
                     "CompanionProtocol"
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
    public void PluginAdapterReportsInventoryAsServerOnlyUnsupported()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var realAdapter = source[..source.IndexOf("#else", StringComparison.Ordinal)];
        var location = SliceMethod(
            realAdapter,
            "public Task<TakaroActionResult> GetPlayerLocationAsync",
            "public Task<TakaroActionResult> GetPlayerInventoryAsync");
        var inventory = SliceMethod(
            realAdapter,
            "public Task<TakaroActionResult> GetPlayerInventoryAsync",
            "public Task<TakaroActionResult> GiveItemAsync");

        var policy = ReadValheimFile("mod/src/Takaro.Valheim.Core/CompanionInventoryActionPolicy.cs");

        StringAssert.Contains(location, "player_position_unavailable");
        Assert.IsFalse(location.Contains("new TakaroPosition(0, 0, 0", StringComparison.Ordinal));
        // Inventory comes only from the optional companion's snapshot; without one the
        // answer is the server_only_unsupported error, never a made-up list.
        StringAssert.Contains(inventory, "CompanionInventoryActionPolicy.FromResolvedPlayer(player, companionInventory");
        StringAssert.Contains(policy, "UnsupportedErrorCode = \"server_only_unsupported\"");
        Assert.IsFalse(inventory.Contains("Array.Empty", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("GetInventory()", StringComparison.Ordinal));
        Assert.IsFalse(inventory.Contains("TryFindPlayerComponent", StringComparison.Ordinal));
    }

    [TestMethod]
    public void PluginPlayerResolverOwnsAuthoritativeIdentityResolution()
    {
        var resolverPath = ValheimPath("mod/src/Takaro.Valheim.Plugin/ValheimPlayerResolver.cs");
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
        var resolverPath = ValheimPath("mod/src/Takaro.Valheim.Plugin/ValheimPlayerResolver.cs");
        Assert.IsTrue(File.Exists(resolverPath), "Missing authoritative Valheim player resolver.");

        var resolver = File.ReadAllText(resolverPath);

        StringAssert.Contains(resolver, "public bool TryResolveConnectedPeer(");
        StringAssert.Contains(resolver, "long sender");
        StringAssert.Contains(resolver, "out ZNetPeer? peer");
        StringAssert.Contains(resolver, "out TakaroPlayer? player");
        StringAssert.Contains(resolver, "PeerResolutionPolicy.TryResolveReadySender(");
        StringAssert.Contains(resolver, "TryFindPlayerInfoForPeer(resolved.Source");
        StringAssert.Contains(resolver, "player = ToTakaroPlayer(playerInfo);");
        StringAssert.Contains(resolver, "candidate.IsReady()");
        Assert.IsFalse(resolver.Contains("player = resolved.Player;", StringComparison.Ordinal));
        Assert.IsFalse(resolver.Contains("payload", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void ServerEventBridgeBindsRoutedPacketsToTheArrivalPeer()
    {
        var bridge = ReadPluginSource("ValheimServerEventBridge.cs");

        StringAssert.Contains(bridge, "[HarmonyPatch(typeof(ZRoutedRpc), \"RPC_RoutedRPC\")]");
        StringAssert.Contains(bridge, "[HarmonyPatch(typeof(Game), \"RPC_RegisterKill\")]");
        StringAssert.Contains(bridge, "arrivalPeer = FindPeer(rpc);");
        StringAssert.Contains(bridge, "ReferenceEquals(peer.m_rpc, rpc)");
        StringAssert.Contains(bridge, "RoutedPacketBindingPolicy.Evaluate(origin, requireOwnCharacter: true)");
        StringAssert.Contains(bridge, "ChatPolicy.Evaluate(origin, characterScoped");
        StringAssert.Contains(bridge, "resolver.TryResolvePeerPlayer(peer, out var player)");
        StringAssert.Contains(bridge, "ZNet.instance.IsDedicated()");
        Assert.IsFalse(bridge.Contains("TryResolveConnectedPeer(", StringComparison.Ordinal));
        Assert.IsFalse(File.Exists(ValheimPath("mod/src/Takaro.Valheim.Plugin/CompanionServerBridge.cs")));
        Assert.IsFalse(File.Exists(ValheimPath("mod/src/Takaro.Valheim.Plugin/ValheimChatEventBridge.cs")));
    }

    [TestMethod]
    public void PluginWiresTheServerEventGraphOnce()
    {
        var entrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");

        StringAssert.Contains(entrypoint, "[\"chatSenderName\"]");
        StringAssert.Contains(entrypoint, "ConnectorConfig.DefaultChatSenderName");
        Assert.IsFalse(entrypoint.Contains("companionMode", StringComparison.OrdinalIgnoreCase));
        StringAssert.Contains(entrypoint, "new InventoryCompanionBridge(playerResolver, companionInventory, Logger.LogInfo)");
        StringAssert.Contains(entrypoint, "new ValheimPlayerResolver(Logger, knownPlayerNames)");
        StringAssert.Contains(entrypoint, "TakaroChatParticipant.Initialize(config.ChatSenderName");

        var resolverAt = entrypoint.IndexOf("new ValheimPlayerResolver(Logger, knownPlayerNames)", StringComparison.Ordinal);
        var adapterAt = entrypoint.IndexOf("new ValheimServerAdapter(", StringComparison.Ordinal);
        var runnerAt = entrypoint.IndexOf("new TakaroWebSocketRunner(", StringComparison.Ordinal);
        var participantAt = entrypoint.IndexOf("TakaroChatParticipant.Initialize(", StringComparison.Ordinal);
        var bridgeAt = entrypoint.IndexOf("ValheimServerEventBridge.Initialize(runner, playerResolver", StringComparison.Ordinal);
        var patchAt = entrypoint.IndexOf("harmony.PatchAll(", StringComparison.Ordinal);
        var startAt = entrypoint.IndexOf("runner.StartAsync()", StringComparison.Ordinal);
        Assert.IsTrue(resolverAt >= 0 && resolverAt < adapterAt);
        Assert.IsTrue(adapterAt < runnerAt);
        Assert.IsTrue(runnerAt < participantAt);
        Assert.IsTrue(participantAt < bridgeAt);
        Assert.IsTrue(bridgeAt < patchAt);
        Assert.IsTrue(patchAt < startAt);

        var destroy = SliceMethod(entrypoint, "private void OnDestroy()", "private void RequestShutdown()");
        Assert.IsTrue(
            destroy.IndexOf("harmony?.UnpatchSelf()", StringComparison.Ordinal)
            < destroy.IndexOf("ValheimServerEventBridge.Shutdown()", StringComparison.Ordinal));
        Assert.IsTrue(
            destroy.IndexOf("ValheimServerEventBridge.Shutdown()", StringComparison.Ordinal)
            < destroy.IndexOf("runner?.Dispose()", StringComparison.Ordinal));
        Assert.IsTrue(
            destroy.IndexOf("runner?.Dispose()", StringComparison.Ordinal)
            < destroy.IndexOf("mainThreadActions?.Dispose()", StringComparison.Ordinal));
    }

    [TestMethod]
    public void RealPluginAdapterTakesResolverAndChatSenderNameFromConfig()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var realAdapter = source[..source.IndexOf("#else", StringComparison.Ordinal)];

        StringAssert.Contains(realAdapter, "private readonly ValheimPlayerResolver playerResolver;");
        StringAssert.Contains(realAdapter, "ValheimPlayerResolver playerResolver,");
        StringAssert.Contains(realAdapter, "CompanionInventoryCache companionInventory)");
        StringAssert.Contains(realAdapter, "this.playerResolver = playerResolver");
        StringAssert.Contains(realAdapter, "chatSenderName = config.ChatSenderName;");
        Assert.IsFalse(realAdapter.Contains("CompanionServerBridge", StringComparison.Ordinal));
        Assert.IsFalse(realAdapter.Contains("TrySendItemGrant", StringComparison.Ordinal));
    }

    [TestMethod]
    public void ServerMessagesUseTheChatParticipantInsteadOfHudOverlays()
    {
        var source = ReadPluginSource("ValheimServerAdapter.cs");
        var send = SliceMethod(
            source,
            "public Task<TakaroActionResult> SendMessageAsync",
            "public Task<TakaroActionResult> ExecuteConsoleCommandAsync");
        var participant = ReadPluginSource("TakaroChatParticipant.cs");

        StringAssert.Contains(send, "if (!TakaroChatParticipant.Active)");
        StringAssert.Contains(send, "chat_participant_unavailable");
        StringAssert.Contains(
            send,
            "var sender = string.IsNullOrWhiteSpace(senderNameOverride) ? chatSenderName : senderNameOverride!.Trim();");
        StringAssert.Contains(send, "TakaroChatParticipant.EnsureName(sender)");
        StringAssert.Contains(send, "TakaroChatParticipant.Send(peer, text)");
        Assert.IsFalse(send.Contains("SendHudMessage", StringComparison.Ordinal));
        Assert.IsFalse(send.Contains("MessageHud", StringComparison.Ordinal));
        Assert.IsFalse(send.Contains("ShowMessage", StringComparison.Ordinal));
        Assert.IsFalse(send.Contains("SendPlayerMessage", StringComparison.Ordinal));

        StringAssert.Contains(participant, "[HarmonyPatch(typeof(ZNet), \"SendPlayerList\")]");
        StringAssert.Contains(participant, "__instance.IsDedicated()");
        StringAssert.Contains(participant, "Write(players, false, null).GetArray().SequenceEqual(vanilla.GetArray())");
        StringAssert.Contains(participant, "return false;");
        StringAssert.Contains(participant, "InvokeRoutedRPC(peer.m_uid, \"ChatMessage\"");
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
        var scheduler = ReadValheimFile("mod/src/Takaro.Valheim.Core/MainThreadActionScheduler.cs");

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
        var sendGameEvent = SliceMethod(
            runner,
            "public Task SendGameEventAsync(",
            "private async Task FlushPendingEventsAsync(");
        StringAssert.Contains(sendGameEvent, "Task.Run(");
        StringAssert.Contains(sendGameEvent, "pendingEvents.Enqueue(TakaroProtocol.CreateGameEvent(eventType, data))");
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
    public void ServerEventBridgeGatesEveryEventThroughTheAcceptancePolicy()
    {
        var source = ReadPluginSource("ValheimServerEventBridge.cs");

        StringAssert.Contains(source, "OnDeathHash");
        StringAssert.Contains(source, "data.m_methodHash == OnDeathHash");
        StringAssert.Contains(
            source,
            "ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.ChatMessage, ValheimEventObservationSource.PeerBoundRoutedRpc)");
        StringAssert.Contains(
            source,
            "ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.PlayerDeath, ValheimEventObservationSource.PeerBoundRoutedRpc)");
        StringAssert.Contains(
            source,
            "ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.EntityKilled, ValheimEventObservationSource.GameKillReport)");
        StringAssert.Contains(
            source,
            "ValheimEventAcceptancePolicy.CanEmit(ValheimEventType.Log, ValheimEventObservationSource.Connector)");
        Assert.IsFalse(source.Contains("EmitPlayerDeathFromRoutedRpc", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"chat-message\"", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"player-death\"", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("\"TakaroPlayerDeath\"", StringComparison.Ordinal));
    }

    [TestMethod]
    public void DestroyZdoIsNotTreatedAsAChatDiagnosticCandidate()
    {
        var source = ReadPluginSource("ValheimServerEventBridge.cs");

        Assert.IsFalse(source.Contains("199378019", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("UndecodedDedicatedServerChatHashes", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("dedicated chat candidate", StringComparison.OrdinalIgnoreCase));
    }

    [TestMethod]
    public void RoutedRpcHookOnlyInspectsChatAndDeathAndRestoresThePackage()
    {
        var source = ReadPluginSource("ValheimServerEventBridge.cs");
        var arrived = SliceMethod(
            source,
            "internal static void OnRoutedRpcArrived(",
            "internal static void OnRoutedRpcDone()");

        StringAssert.Contains(
            arrived,
            "data.m_methodHash != SayHash && data.m_methodHash != ChatMessageHash && data.m_methodHash != OnDeathHash && data.m_methodHash != RegisterKillHash");
        StringAssert.Contains(arrived, "var start = package.GetPos();");
        StringAssert.Contains(arrived, "finally");
        StringAssert.Contains(arrived, "package.SetPos(start);");
        StringAssert.Contains(source, "private static void Finalizer() => ValheimServerEventBridge.OnRoutedRpcDone();");
    }

    [TestMethod]
    public void EntityKilledComesFromTheGamesOwnKillReports()
    {
        var source = ReadPluginSource("ValheimServerEventBridge.cs");
        var observe = SliceMethod(
            source,
            "private static void ObserveKillReport(",
            "private const string OwnKill");
        var participant = ReadPluginSource("TakaroChatParticipant.cs");

        StringAssert.Contains(observe, "RoutedPacketBindingPolicy.Evaluate(origin, requireOwnCharacter: false)");
        StringAssert.Contains(observe, "TakaroChatParticipant.IsKillWitnessTarget(data.m_targetPeerID)");
        StringAssert.Contains(observe, "CompanionKillVerdicts.HasSession(killer.m_uid)");
        StringAssert.Contains(source, "[HarmonyPatch(typeof(Game), \"RPC_RegisterKill\")]");
        StringAssert.Contains(participant, "WitnessFor(peer, players)");
        Assert.IsFalse(source.Contains("ObserveDestroyed", StringComparison.Ordinal));
        Assert.IsFalse(source.Contains("TakaroCharacterOnDeathPatch", StringComparison.Ordinal));
    }

    [TestMethod]
    public void GameEventsAreQueuedAndOnlyFlushedAfterIdentify()
    {
        var source = ReadPluginSource("TakaroWebSocketRunner.cs");

        StringAssert.Contains(source, "lifecycle event queued");
        Assert.IsFalse(source.Contains("event sent for", StringComparison.Ordinal));
        StringAssert.Contains(source, "while (identified && pendingEvents.TryPeekUnsent(out var frame))");
        StringAssert.Contains(source, "pendingEvents.MarkSent(frame);");
        StringAssert.Contains(source, "pendingEvents.ConfirmOldestCheckpoint();");
        StringAssert.Contains(source, "pendingEvents.ResetForNewConnection();");
        StringAssert.Contains(source, "activeSocket.Abort();");
        var identifiedAt = source.IndexOf("if (LogIdentifyResponse(message))", StringComparison.Ordinal);
        Assert.IsTrue(identifiedAt >= 0);
        Assert.IsTrue(source.IndexOf("identified = true;", identifiedAt, StringComparison.Ordinal) > identifiedAt);
        StringAssert.Contains(source, "TakaroProtocol.TryCreateActionResponse");
    }

    [TestMethod]
    public void ObsoleteClientSnapshotCachesAndTestsAreRemoved()
    {
        var inventory = ReadValheimFile("mod/src/Takaro.Valheim.Core/Inventory.cs");
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
    public void TheOnlyClientModIsTheOptionalInventoryCompanion()
    {
        var readme = ReadValheimFile("README.md");
        var connector = ReadValheimFile("connector.json");
        var solution = ReadValheimFile("mod/Takaro.Valheim.sln");
        var release = ReadValheimFile("scripts/build-release.sh");
        var config = ReadValheimFile("mod/src/Takaro.Valheim.Core/ConnectorConfig.cs");
        var serverEntrypoint = ReadPluginSource("ValheimTakaroPlugin.cs");

        StringAssert.Contains(readme, "takaro-valheim-plugin.zip");
        StringAssert.Contains(readme, "takaro-valheim-inventory-companion.zip");
        StringAssert.Contains(serverEntrypoint, "if (!IsDedicatedServerProcess())");
        StringAssert.Contains(solution, "Takaro.Valheim.Companion.csproj");
        StringAssert.Contains(release, "EnableValheimCompanionBuild=true");
        Assert.IsFalse(config.Contains("companion", StringComparison.OrdinalIgnoreCase));
        Assert.IsFalse(File.Exists(ValheimPath("COMPANION.md")));

        using var document = System.Text.Json.JsonDocument.Parse(connector);
        var companionAssets = document.RootElement.GetProperty("assets").EnumerateArray()
            .Where(asset => asset.GetProperty("pattern").GetString()!.Contains("companion", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        Assert.AreEqual(1, companionAssets.Length);
        Assert.AreEqual("takaro-valheim-inventory-companion.zip", companionAssets[0].GetProperty("pattern").GetString());
        Assert.IsFalse(companionAssets[0].GetProperty("required").GetBoolean());
    }

    [TestMethod]
    public void ServerOnlyPluginHasNoJotunnDependencyAndRetriesReferenceSetup()
    {
        var project = ReadValheimFile("mod/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj");
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

        StringAssert.Contains(setup, "VALHEIM_REFERENCE_CACHE_DIR");
        StringAssert.Contains(setup, ".takaro-valheim-reference-cache");
        StringAssert.Contains(setup, "refusing to mutate");
        StringAssert.Contains(setup, "valheim_server_Data/Managed");
        StringAssert.Contains(setup, "--retry 5");
        StringAssert.Contains(setup, "--retry-delay 2");
        StringAssert.Contains(setup, "--retry-all-errors");
        StringAssert.Contains(setup, "command -v file");
        StringAssert.Contains(setup, "requires the 'file' command");
        StringAssert.Contains(setup, "Mono/.Net\\ assembly");

        // Both inputs come from the target and from nowhere else: the references from the
        // pinned depot manifest, the pack from its exact version URL. There is no platform
        // loop, no SteamCMD and no Thunderstore 'latest' -- a pinned build Steam no longer
        // serves is a re-pin, not a retry somewhere else.
        StringAssert.Contains(setup, "steam references");
        StringAssert.Contains(setup, "not falling back");
        StringAssert.Contains(setup, "VALHEIM_BEPINEX_SHA256");
        StringAssert.Contains(setup, "VALHEIM_BEPINEX_PACK_VERSION");
        foreach (var gone in new[]
        {
            "VALHEIM_STEAM_PLATFORMS",
            "app_update",
            "STEAMCMD",
            "latest.download_url",
        })
        {
            Assert.IsFalse(setup.Contains(gone, StringComparison.OrdinalIgnoreCase), gone);
        }
    }

    [TestMethod]
    public void ReleaseCachingIsKeyedOnTheTargetFingerprintNotOnAFixedDirectory()
    {
        var workflow = ReadRepositoryFile(".github/workflows/valheim.yml");
        var shared = ReadRepositoryFile(".github/workflows/connector-release.yml");

        // Inputs live under their target's fingerprint, so a re-pin can never be served
        // another build's assemblies from cache. A cache keyed on a fixed directory would
        // share one entry across every server build.
        foreach (var gone in new[] { "_data/server", "valheim-build-deps" })
        {
            Assert.IsFalse(workflow.Contains(gone, StringComparison.Ordinal), gone);
        }

        StringAssert.Contains(shared, "steps.target.outputs.fp16");
    }

    private static string ReadRepositoryFile(string relativePath) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../../../",
            relativePath)));

    private static string ReadPluginSource(string fileName)
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../mod/src/Takaro.Valheim.Plugin",
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
