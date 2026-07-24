import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = REPOSITORY_ROOT / "7d2d"


class SourceRegressionTests(unittest.TestCase):
    def source(self, relative_path: str) -> str:
        return (SOURCE_ROOT / relative_path).read_text()

    def test_current_ban_sources_are_mirrored(self):
        mirror = self.source("src/Services/StateMirror.cs")
        self.assertIn("GameManager.Instance.adminTools.Blacklist.GetBanned()", mirror)
        self.assertIn("Platform.BlockedPlayerList.Instance.GetEntriesOrdered", mirror)
        self.assertIn("Shared.TransformBanRecordToTakaroBan(record)", mirror)

    def test_timed_bans_use_game_local_deadlines_and_persist_before_kick(self):
        actions = self.source("src/WebSocket/ActionHandlers.cs")
        ban_player = actions.split(
            "public static async Task BanPlayer", 1
        )[1].split("public static async Task UnbanPlayer", 1)[0]
        mirror = self.source("src/Services/StateMirror.cs")

        self.assertIn("BanExpiry.TryCreateGameDeadline", ban_player)
        self.assertIn("DateTimeOffset.UtcNow", ban_player)
        self.assertIn("TimeZoneInfo.Local", ban_player)
        self.assertIn("Blacklist.AddBan", ban_player)
        self.assertIn("Blacklist.IsBanned", ban_player)
        self.assertIn("Game did not persist timed ban", ban_player)
        self.assertLess(
            ban_player.index("Blacklist.IsBanned"),
            ban_player.index("GameUtils.KickPlayerForClientInfo"),
        )
        self.assertNotIn("banUntil.ToUniversalTime()", ban_player)
        self.assertIn("BanExpiry.ToTakaroUtc", mirror)

    def test_reconnect_remains_indefinite_with_a_backoff_cap(self):
        transport = self.source("src/WebSocket/WebSocketTransport.cs")
        self.assertNotIn("MAX_RECONNECT_ATTEMPTS", transport)
        self.assertNotIn("Giving up", transport)
        self.assertIn("_reconnectAttempts++", transport)
        self.assertIn("Math.Pow(2", transport)
        self.assertIn("MAX_RECONNECT_INTERVAL_SECONDS", transport)

    def test_native_game_logs_are_forwarded_without_connector_feedback(self):
        api = self.source("src/API.cs")
        self.assertIn("Log.LogCallbacksExtended += HandleNativeLogMessage", api)
        self.assertIn("Log.LogCallbacksExtended -= HandleNativeLogMessage", api)
        self.assertNotIn("Application.logMessageReceived", api)
        self.assertIn("GameEventPublisher.SendLogEvent(plainMessage)", api)
        self.assertIn('plainMessage.Contains($"[{ModPrefix}]")', api)
        self.assertIn("ServerMessageEchoGuard.Instance.ShouldSuppress", api)
        self.assertNotIn("type == LogType.Error || type == LogType.Warning", api)

        actions = self.source("src/WebSocket/ActionHandlers.cs")
        send_message = actions.split(
            "public static async Task SendChatMessage", 1
        )[1].split("public static async Task ExecuteCommand", 1)[0]
        self.assertIn("ServerMessageEchoGuard.Instance.Record(renderedMessage)", send_message)
        self.assertLess(
            send_message.index("ServerMessageEchoGuard.Instance.Record(renderedMessage)"),
            send_message.index("GameManager.Instance.ChatMessageServer"),
        )

    def test_lifecycle_events_use_stable_v3_server_owned_hooks(self):
        api = self.source("src/API.cs")
        disconnect = api.split(
            "private static void PlayerDisconnected", 1
        )[1].split("public void EntityKilled", 1)[0]
        entity_killed = api.split("public void EntityKilled", 1)[1].split(
            "private static ModEvents.EModEventResult GameMessage", 1
        )[0]

        self.assertIn("TransformClientInfoToTakaroPlayerIdentity", disconnect)
        self.assertIn("GameEventPublisher.SendPlayerDisconnected(player)", disconnect)
        self.assertLess(
            disconnect.index("TransformClientInfoToTakaroPlayerIdentity"),
            disconnect.index("StateMirror.Instance.MarkOffline"),
        )

        self.assertIn("ModEvents.GameMessage.RegisterHandler(GameMessage)", api)
        self.assertIn("ModEvents.GameMessage.UnregisterHandler(GameMessage)", api)
        self.assertIn("EnumGameMessages.EntityWasKilled", api)
        self.assertIn("GameEventPublisher.SendPlayerDeath", api)
        self.assertIn("ModEvents.EModEventResult.Continue", api)
        game_message = api.split(
            "private static ModEvents.EModEventResult GameMessage", 1
        )[1].split("private static void PlayerSpawnedInWorld", 1)[0]
        self.assertIn("data.MainName", game_message)
        self.assertIn("PlayerDisplayName", game_message)
        self.assertIn("Clients.ForEntityId", game_message)
        self.assertNotIn("|| data.ClientInfo == null", game_message)

        self.assertIn(
            "data.KilledEntitiy.entityType == EntityType.Player", entity_killed
        )
        self.assertIn("return;", entity_killed)
        self.assertNotIn("Player death:", entity_killed)
        self.assertNotIn("GameEventPublisher.SendPlayerDeath", entity_killed)
        self.assertIn("GameEventPublisher.SendEntityKilled", entity_killed)

    def test_default_endpoint_is_production(self):
        config = self.source("src/Config/ConfigManager.cs")
        readme = self.source("README.md")
        self.assertIn("wss://connect.takaro.io/", config)
        self.assertNotIn("wss://your-takaro-websocket-server.com", config)
        self.assertIn("wss://connect.takaro.io/", readme)

    def test_sensitive_payloads_are_not_logged(self):
        router = self.source("src/WebSocket/RequestRouter.cs")
        transport = self.source("src/WebSocket/WebSocketTransport.cs")
        config = self.source("src/Config/ConfigManager.cs")
        self.assertNotIn('LogService.Instance.Debug($"Received: {message}")', router)
        self.assertNotIn("serializedMessage", transport)
        self.assertNotIn("Identity token: {identityToken}", config)

    def test_protocol_error_frames_are_diagnosed_without_dispatch_or_raw_payloads(self):
        router = self.source("src/WebSocket/RequestRouter.cs")
        error_branch = router.split(
            "webSocketMessage.Type == WebSocketMessage.MessageTypes.Error", 1
        )[1].split(
            "webSocketMessage.Type != WebSocketMessage.MessageTypes.Request", 1
        )[0]
        diagnostics = self.source("src/Services/ProtocolDiagnostics.cs")

        self.assertIn("ProtocolDiagnostics.ExtractErrorMessage", error_branch)
        self.assertIn("LogService.Instance.Warn", error_branch)
        self.assertIn("return;", error_branch)
        self.assertNotIn("Dispatch(", error_branch)
        self.assertIn("MaxMessageLength", diagnostics)
        self.assertIn('jObject["message"]', diagnostics)
        self.assertNotIn("SerializeObject(payload)", diagnostics)

    def test_console_command_responses_reflect_native_error_lines(self):
        actions = self.source("src/WebSocket/ActionHandlers.cs")
        execute_command = actions.split("public static async Task ExecuteCommand", 1)[
            1
        ].split("public static async Task KickPlayer", 1)[0]
        classifier = self.source("src/Services/ConsoleCommandOutcome.cs")

        self.assertIn("ConsoleCommandOutcome.FromRawResult", execute_command)
        self.assertIn('{ "success", outcome.Success }', execute_command)
        self.assertIn('payload["errorMessage"] = outcome.ErrorMessage', execute_command)
        self.assertNotIn('{ "success", true }', execute_command)
        self.assertIn('"*** ERROR:"', classifier)
        self.assertIn('"Wrong number of arguments"', classifier)
        self.assertIn('"Invalid value for"', classifier)
        self.assertIn("StartsWith(prefix, StringComparison.Ordinal)", classifier)
        self.assertIn("MaxErrorMessageLength", classifier)

    def test_production_handlers_preserve_supported_not_found_semantics(self):
        reads = self.source("src/WebSocket/ReadHandlers.cs")
        actions = self.source("src/WebSocket/ActionHandlers.cs")
        give_item = self.source("src/WebSocket/GiveItemHandler.cs")
        self.assertEqual(0, reads.count("WebSocketMessage.CreateResponse(requestId, null)"))
        self.assertEqual(2, reads.count('SendError(requestId, "Player not found")'))
        self.assertIn("new TakaroItem[0]", reads)
        self.assertEqual(6, actions.count("WebSocketMessage.CreateResponse(requestId, null)"))
        self.assertEqual(1, give_item.count("WebSocketMessage.CreateResponse(requestId, null)"))

    def test_give_item_uses_only_the_first_party_world_drop_delivery_seam(self):
        actions = self.source("src/WebSocket/ActionHandlers.cs")
        self.assertIn("GiveItemHandler.Handle(requestId, args)", actions)
        give_item = self.source("src/WebSocket/GiveItemHandler.cs")
        self.assertNotIn("player.GetPosition()", give_item)
        self.assertNotIn("NetPackageEntityCollect", give_item)
        self.assertNotIn("EntityFactory.CreateEntity", give_item)
        self.assertNotIn("RemoveEntity", give_item)
        self.assertIn("PlayerProximateItemDelivery.Drop(iv, args.Amount, player)", give_item)
        delivery = self.source("src/Services/PlayerProximateItemDelivery.cs")
        self.assertIn("EntityPlayer player", delivery)
        self.assertIn("player.GetDropPosition()", delivery)
        self.assertNotIn("player.entityId", delivery)
        self.assertIn("Vector3.zero", delivery)

    def test_missing_inventory_is_an_array(self):
        reads = self.source("src/WebSocket/ReadHandlers.cs")
        self.assertIn("StateMirror.Instance.GetOnlinePlayer(gameId)", reads)
        self.assertIn("StateMirror.Instance.GetPlayerInventory(gameId)", reads)
        self.assertIn("new TakaroItem[0]", reads)

    def test_world_catalogues_have_plain_mirror_records(self):
        shared = self.source("src/Shared.cs")
        records = self.source("src/Persistence/Records.cs")
        database = self.source("src/Persistence/Database.cs")
        self.assertIn("public class TakaroEntity", shared)
        self.assertIn("public class TakaroLocation", shared)
        self.assertIn("public class TakaroPosition", shared)
        self.assertIn("public class EntityRecord", records)
        self.assertIn("public class LocationRecord", records)
        self.assertIn("ILiteCollection<EntityRecord> Entities", database)
        self.assertIn("ILiteCollection<LocationRecord> Locations", database)
        self.assertNotIn("UnityEngine", records)

    def test_game_thread_seed_captures_current_v3_catalogues(self):
        mirror = self.source("src/Services/StateMirror.cs")
        self.assertIn("EntityClass.list.Dict", mirror)
        self.assertIn("UserSpawnType.None", mirror)
        self.assertIn("typeof(EntityAlive)", mirror)
        self.assertIn("typeof(EntityPlayer)", mirror)
        self.assertIn("typeof(EntityVehicle)", mirror)
        self.assertIn("GameManager.Instance.GetDynamicPrefabDecorator()", mirror)
        self.assertIn("GetPOIPrefabs", mirror)
        self.assertIn("boundingBoxPosition", mirror)
        self.assertIn("boundingBoxSize", mirror)
        self.assertIn('PositionAnchor = "min-corner"', mirror)
        self.assertIn("records.Sort", mirror)

    def test_world_read_handlers_use_only_deterministic_mirror_snapshots(self):
        reads = self.source("src/WebSocket/ReadHandlers.cs")
        mirror = self.source("src/Services/StateMirror.cs")
        self.assertIn("StateMirror.Instance.GetEntities()", reads)
        self.assertIn("StateMirror.Instance.GetLocations()", reads)
        self.assertIn("public List<TakaroEntity> GetEntities()", mirror)
        self.assertIn("public List<TakaroLocation> GetLocations()", mirror)
        self.assertNotIn("EntityClass.", reads)
        self.assertNotIn("GameManager.", reads)
        self.assertNotIn("PrefabInstance", reads)
        self.assertNotIn("new object[0]", reads)

    def test_item_catalogue_reads_are_deterministically_sorted(self):
        mirror = self.source("src/Services/StateMirror.cs")
        get_items = mirror.split("public List<TakaroItem> GetItems()", 1)[1].split(
            "public List<TakaroBan> GetBans()", 1
        )[0]
        self.assertIn("records.Sort", get_items)

    def test_reachability_reads_cached_game_readiness(self):
        reads = self.source("src/WebSocket/ReadHandlers.cs")
        mirror = self.source("src/Services/StateMirror.cs")
        api = self.source("src/API.cs")
        self.assertIn("StateMirror.Instance.IsGameReady", reads)
        self.assertIn("public bool IsGameReady", mirror)
        self.assertIn("StateMirror.Instance.MarkGameReady()", api)
        self.assertIn("StateMirror.Instance.MarkGameStopping()", api)

    def test_startup_waits_for_seed_writes_before_advertising_readiness(self):
        api = self.source("src/API.cs")
        startup = api.split(
            "private static void GameStartDone", 1
        )[1].split("private static void GameUpdate", 1)[0]
        writer = self.source("src/Services/DbWriter.cs")

        self.assertIn("DbWriter.Instance.Flush", startup)
        self.assertLess(
            startup.index("StateMirror.Instance.SeedOnGameStart()"),
            startup.index("DbWriter.Instance.Flush"),
        )
        self.assertLess(
            startup.index("DbWriter.Instance.Flush"),
            startup.index("StateMirror.Instance.MarkGameReady()"),
        )
        self.assertLess(
            startup.index("StateMirror.Instance.MarkGameReady()"),
            startup.index("WebSocketTransport.Instance.Initialize()"),
        )
        self.assertIn("public void Flush(TimeSpan timeout)", writer)
        self.assertIn("TimeoutException", writer)
        self.assertIn("_hasFailedOperation", writer)


if __name__ == "__main__":
    unittest.main()
