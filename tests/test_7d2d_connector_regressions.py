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

    def test_reconnect_remains_indefinite_with_a_backoff_cap(self):
        transport = self.source("src/WebSocket/WebSocketTransport.cs")
        self.assertNotIn("MAX_RECONNECT_ATTEMPTS", transport)
        self.assertNotIn("Giving up", transport)
        self.assertIn("_reconnectAttempts++", transport)
        self.assertIn("Math.Pow(2", transport)
        self.assertIn("MAX_RECONNECT_INTERVAL_SECONDS", transport)

    def test_raw_game_logs_are_forwarded_without_connector_feedback(self):
        api = self.source("src/API.cs")
        self.assertIn("GameEventPublisher.SendLogEvent(logString)", api)
        self.assertIn('logString.Contains($"[{ModPrefix}]")', api)
        self.assertNotIn("type == LogType.Error || type == LogType.Warning", api)

    def test_default_endpoint_is_production(self):
        config = self.source("src/Config/ConfigManager.cs")
        readme = self.source("README.md")
        self.assertIn("wss://connect.takaro.io/", config)
        self.assertNotIn("wss://your-takaro-websocket-server.com", config)
        self.assertIn("wss://connect.takaro.io/", readme)

    def test_ci_managed_assemblies_are_pinned_to_the_live_proven_v3_build(self):
        setup = self.source("scripts/setup-environment.sh")
        workflow = (REPOSITORY_ROOT / ".github/workflows/7d2d.yml").read_text()
        expected_hash = "d05257aa0a597abe51b39574fc86acd5945da4d5e41b66b7f357e0c2ea5e55bd"
        self.assertIn(expected_hash, setup)
        self.assertIn("verify_managed_assembly", setup)
        self.assertIn("7d2d-managed-v3-0-1-b24117900-v1", workflow)

    def test_sensitive_payloads_are_not_logged(self):
        router = self.source("src/WebSocket/RequestRouter.cs")
        transport = self.source("src/WebSocket/WebSocketTransport.cs")
        config = self.source("src/Config/ConfigManager.cs")
        self.assertNotIn('LogService.Instance.Debug($"Received: {message}")', router)
        self.assertNotIn("serializedMessage", transport)
        self.assertNotIn("Identity token: {identityToken}", config)

    def test_production_handlers_preserve_null_semantics(self):
        reads = self.source("src/WebSocket/ReadHandlers.cs")
        actions = self.source("src/WebSocket/ActionHandlers.cs")
        give_item = self.source("src/WebSocket/GiveItemHandler.cs")
        self.assertEqual(2, reads.count("WebSocketMessage.CreateResponse(requestId, null)"))
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


if __name__ == "__main__":
    unittest.main()
