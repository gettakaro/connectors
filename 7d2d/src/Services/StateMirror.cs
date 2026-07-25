using System;
using System.Collections.Generic;
using System.Globalization;
using Takaro.Interfaces;
using Takaro.Persistence;

namespace Takaro.Services
{
    /// <summary>
    /// Event-driven mirror of game state in LiteDB. Write methods run on the game
    /// main thread, capture plain POCO snapshots and enqueue the DB work onto the
    /// DbWriter thread. Read methods serve Takaro requests on the WebSocket thread
    /// and never touch game APIs.
    /// </summary>
    public class StateMirror : IService
    {
        private static StateMirror _instance;
        private static readonly object _lock = new object();
        private volatile bool _isGameReady;

        public bool IsGameReady => _isGameReady;

        public static StateMirror Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new StateMirror();
                }
                return _instance;
            }
        }

        public void OnInit() { }

        public void OnDestroy() { }

        public void MarkGameReady()
        {
            _isGameReady = true;
        }

        public void MarkGameStopping()
        {
            _isGameReady = false;
        }

        #region Read side (WebSocket thread)

        public List<TakaroPlayer> GetOnlinePlayers()
        {
            var players = new List<TakaroPlayer>();
            lock (Database.Instance.SyncRoot)
            {
                foreach (PlayerRecord record in Database.Instance.Players.Find(p => p.Online))
                    players.Add(Shared.TransformPlayerRecordToTakaroPlayer(record));
            }
            return players;
        }

        public PlayerRecord GetOnlinePlayer(string gameId)
        {
            PlayerRecord record;
            lock (Database.Instance.SyncRoot)
            {
                record = Database.Instance.Players.FindById(gameId);
            }
            return record != null && record.Online ? record : null;
        }

        public PlayerRecord GetPlayerLocationRecord(string gameId)
        {
            PlayerRecord record;
            lock (Database.Instance.SyncRoot)
            {
                record = Database.Instance.Players.FindById(gameId);
            }

            if (record == null)
                return null;

            return PlayerLocationReadWindow.IsReadable(
                record.Online,
                record.LastSeenUtc,
                DateTime.UtcNow
            )
                ? record
                : null;
        }

        public List<TakaroItem> GetPlayerInventory(string gameId)
        {
            InventoryRecord record;
            lock (Database.Instance.SyncRoot)
            {
                record = Database.Instance.Inventories.FindById(gameId);
            }
            if (record == null)
                return new List<TakaroItem>();

            var items = new List<TakaroItem>();
            foreach (ItemSlot slot in record.Items)
            {
                items.Add(
                    new TakaroItem
                    {
                        Code = slot.Code,
                        Name = slot.Name,
                        Description = slot.Description,
                        Amount = slot.Amount,
                        Quality = slot.Quality,
                    }
                );
            }
            return items;
        }

        public List<TakaroEntity> GetEntities()
        {
            List<EntityRecord> records;
            lock (Database.Instance.SyncRoot)
            {
                records = new List<EntityRecord>(Database.Instance.Entities.FindAll());
            }
            records.Sort((left, right) => string.CompareOrdinal(left.Code, right.Code));

            var entities = new List<TakaroEntity>();
            foreach (EntityRecord record in records)
            {
                var metadata = new Dictionary<string, object>
                {
                    { "runtimeClass", record.RuntimeClass },
                    { "spawnType", record.SpawnType },
                };
                entities.Add(
                    new TakaroEntity
                    {
                        Code = record.Code,
                        Name = record.Name,
                        Description = record.Description,
                        Type = record.Type,
                        Metadata = metadata,
                    }
                );
            }
            return entities;
        }

        public List<TakaroLocation> GetLocations()
        {
            List<LocationRecord> records;
            lock (Database.Instance.SyncRoot)
            {
                records = new List<LocationRecord>(Database.Instance.Locations.FindAll());
            }
            records.Sort((left, right) => string.CompareOrdinal(left.Code, right.Code));

            var locations = new List<TakaroLocation>();
            foreach (LocationRecord record in records)
            {
                locations.Add(
                    new TakaroLocation
                    {
                        Code = record.Code,
                        Name = record.Name,
                        Position = new TakaroPosition
                        {
                            X = record.X,
                            Y = record.Y,
                            Z = record.Z,
                        },
                        SizeX = record.SizeX,
                        SizeY = record.SizeY,
                        SizeZ = record.SizeZ,
                        Metadata = new Dictionary<string, object>
                        {
                            { "prefab", record.PrefabName },
                            { "rotation", record.Rotation },
                            { "positionAnchor", record.PositionAnchor },
                        },
                    }
                );
            }
            return locations;
        }

        public List<TakaroItem> GetItems()
        {
            List<ItemRecord> records;
            lock (Database.Instance.SyncRoot)
            {
                records = new List<ItemRecord>(Database.Instance.Items.FindAll());
            }
            records.Sort((left, right) => string.CompareOrdinal(left.Code, right.Code));

            var items = new List<TakaroItem>();
            foreach (ItemRecord record in records)
            {
                items.Add(
                    new TakaroItem
                    {
                        Code = record.Code,
                        Name = record.Name,
                        Description = record.Description,
                    }
                );
            }
            return items;
        }

        public List<TakaroBan> GetBans()
        {
            List<BanRecord> records;
            lock (Database.Instance.SyncRoot)
            {
                records = new List<BanRecord>(Database.Instance.Bans.FindAll());
            }

            var bans = new List<TakaroBan>();
            foreach (BanRecord record in records)
            {
                bans.Add(Shared.TransformBanRecordToTakaroBan(record));
            }
            return bans;
        }

        #endregion

        #region Write side (game main thread)

        /// <summary>
        /// Seeds the mirror from game truth at GameStartDone, before the WebSocket
        /// connects — requests can never observe a cold mirror.
        /// </summary>
        public void SeedOnGameStart()
        {
            SeedItems();
            SeedEntities();
            SeedLocations();
            RefreshBans();
            LogService.Instance.Info(
                "State mirror seeding enqueued (items, entities, locations, bans)"
            );
        }

        public void UpsertPlayerOnline(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null)
                return;

            PlayerRecord record = BuildPlayerRecord(cInfo);
            record.Online = true;

            if (
                GameManager.Instance.World.Players.dict.TryGetValue(
                    cInfo.entityId,
                    out EntityPlayer entity
                )
            )
            {
                UnityEngine.Vector3 position = entity.GetPosition();
                record.X = position.x;
                record.Y = position.y;
                record.Z = position.z;
            }

            DbWriter.Instance.Enqueue(() => Database.Instance.Players.Upsert(record));
        }

        public void MarkOffline(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null)
                return;

            string gameId = Shared.GameIdFromClientInfo(cInfo);
            DbWriter.Instance.Enqueue(() =>
            {
                PlayerRecord record = Database.Instance.Players.FindById(gameId);
                if (record == null)
                    return;
                record.Online = false;
                record.LastSeenUtc = DateTime.UtcNow;
                Database.Instance.Players.Update(record);
            });
        }

        public void UpdatePositions(List<PositionSample> batch)
        {
            DbWriter.Instance.Enqueue(() =>
            {
                foreach (PositionSample sample in batch)
                {
                    PlayerRecord record = Database.Instance.Players.FindById(sample.GameId);
                    if (record == null || !record.Online)
                        continue;
                    record.X = sample.X;
                    record.Y = sample.Y;
                    record.Z = sample.Z;
                    record.Ping = sample.Ping;
                    record.LastSeenUtc = DateTime.UtcNow;
                    Database.Instance.Players.Update(record);
                }
            });
        }

        public void UpsertInventory(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null || cInfo.latestPlayerData == null)
                return;

            var slots = new List<ItemSlot>();
            CaptureItemStacks(cInfo.latestPlayerData.inventory, slots);
            CaptureItemStacks(cInfo.latestPlayerData.bag?.GetSlots(), slots);
            CaptureEquippedItems(cInfo.latestPlayerData.equipment?.GetItems(), slots);

            string gameId = Shared.GameIdFromClientInfo(cInfo);
            DbWriter.Instance.Enqueue(
                () =>
                    Database.Instance.Inventories.Upsert(
                        new InventoryRecord
                        {
                            GameId = gameId,
                            Items = slots,
                            UpdatedUtc = DateTime.UtcNow,
                        }
                    )
            );
        }

        /// <summary>
        /// Captures the full ban list from game truth (AdminTools blacklist merged
        /// with the platform BlockedPlayerList) and replaces the bans collection.
        /// Runs at seed time, after Takaro ban/unban actions, and on a periodic
        /// resync to catch console-issued bans.
        /// </summary>
        public void RefreshBans()
        {
            List<BanRecord> records = CaptureBans();
            LogService.Instance.Debug($"Ban resync captured {records.Count} entries");
            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Bans.DeleteAll();
                if (records.Count > 0)
                    Database.Instance.Bans.InsertBulk(records);
            });
        }

        private void SeedItems()
        {
            var records = new List<ItemRecord>();
            // ItemClass.itemNames can map several entries to the same item code;
            // the collection is keyed by code, so keep the first occurrence.
            var seenCodes = new HashSet<string>();
            for (int i = 0; i < ItemClass.itemNames.Count; i++)
            {
                string itemName = ItemClass.itemNames[i];
                ItemClass item = ItemClass.nameToItem[itemName];
                if (item == null || !seenCodes.Add(item.GetItemName()))
                    continue;

                records.Add(
                    new ItemRecord
                    {
                        Code = item.GetItemName(),
                        Name = item.GetLocalizedItemName(),
                        Description = Localization.Get($"{item.GetItemName()}Desc", true),
                    }
                );
            }

            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Items.DeleteAll();
                Database.Instance.Items.InsertBulk(records);
            });
        }

        private void SeedEntities()
        {
            var records = new List<EntityRecord>();
            var seenCodes = new HashSet<string>();

            foreach (KeyValuePair<int, EntityClass> entry in EntityClass.list.Dict)
            {
                EntityClass entityClass = entry.Value;
                if (
                    entityClass == null
                    || entityClass.userSpawnType == EntityClass.UserSpawnType.None
                    || entityClass.classname == null
                    || !typeof(EntityAlive).IsAssignableFrom(entityClass.classname)
                    || typeof(EntityPlayer).IsAssignableFrom(entityClass.classname)
                    || typeof(EntityVehicle).IsAssignableFrom(entityClass.classname)
                )
                    continue;

                string code = entityClass.entityClassName;
                if (string.IsNullOrEmpty(code) || !seenCodes.Add(code))
                    continue;

                string localizedName = Localization.Get(code, true);
                if (string.IsNullOrEmpty(localizedName))
                    localizedName = code;

                records.Add(
                    new EntityRecord
                    {
                        Code = code,
                        Name = localizedName,
                        Type = entityClass.bIsEnemyEntity ? "hostile" : null,
                        RuntimeClass = entityClass.classname.Name,
                        SpawnType = entityClass.userSpawnType.ToString(),
                    }
                );
            }

            records.Sort((left, right) => string.CompareOrdinal(left.Code, right.Code));
            LogService.Instance.Info($"Entity catalogue captured {records.Count} entries");
            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Entities.DeleteAll();
                if (records.Count > 0)
                    Database.Instance.Entities.InsertBulk(records);
            });
        }

        private void SeedLocations()
        {
            var records = new List<LocationRecord>();
            var seenCodes = new HashSet<string>();
            DynamicPrefabDecorator decorator = GameManager.Instance.GetDynamicPrefabDecorator();
            if (decorator != null)
            {
                var prefabs = new List<PrefabInstance>();
                decorator.GetPOIPrefabs(prefabs);
                foreach (PrefabInstance instance in prefabs)
                {
                    if (instance?.prefab == null)
                        continue;

                    string prefabName = instance.prefab.PrefabName;
                    if (string.IsNullOrEmpty(prefabName))
                        prefabName = instance.name;
                    if (
                        string.IsNullOrEmpty(prefabName)
                        || instance.boundingBoxSize.x <= 0
                        || instance.boundingBoxSize.y <= 0
                        || instance.boundingBoxSize.z <= 0
                    )
                        continue;

                    string code = string.Format(
                        CultureInfo.InvariantCulture,
                        "{0}@{1},{2},{3}:r{4}",
                        prefabName,
                        instance.boundingBoxPosition.x,
                        instance.boundingBoxPosition.y,
                        instance.boundingBoxPosition.z,
                        instance.rotation
                    );
                    if (!seenCodes.Add(code))
                        continue;

                    string localizedName = instance.prefab.LocalizedName;
                    if (string.IsNullOrEmpty(localizedName))
                        localizedName = prefabName;

                    records.Add(
                        new LocationRecord
                        {
                            Code = code,
                            Name = localizedName,
                            X = instance.boundingBoxPosition.x,
                            Y = instance.boundingBoxPosition.y,
                            Z = instance.boundingBoxPosition.z,
                            SizeX = instance.boundingBoxSize.x,
                            SizeY = instance.boundingBoxSize.y,
                            SizeZ = instance.boundingBoxSize.z,
                            PrefabName = prefabName,
                            Rotation = instance.rotation,
                            PositionAnchor = "min-corner",
                        }
                    );
                }
            }

            records.Sort((left, right) => string.CompareOrdinal(left.Code, right.Code));
            LogService.Instance.Info($"Location catalogue captured {records.Count} entries");
            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Locations.DeleteAll();
                if (records.Count > 0)
                    Database.Instance.Locations.InsertBulk(records);
            });
        }

        private static List<BanRecord> CaptureBans()
        {
            var records = new List<BanRecord>();
            var seenBanIds = new HashSet<string>();
            PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();

            // AdminTools.Blacklist stores timed bans and preserves reason/expiry metadata.
            if (GameManager.Instance?.adminTools?.Blacklist != null)
            {
                foreach (var ban in GameManager.Instance.adminTools.Blacklist.GetBanned())
                {
                    if (ban.UserIdentifier == null)
                        continue;

                    string banId = ban.UserIdentifier.CombinedString;
                    if (string.IsNullOrEmpty(banId) || seenBanIds.Contains(banId))
                        continue;

                    PersistentPlayerData playerData = playerList?.GetPlayerData(ban.UserIdentifier);
                    string playerName =
                        playerData != null
                            ? playerData.PlayerName.playerName.Text
                            : $"Player_{banId.Replace("EOS_", "")}";

                    var record = new BanRecord
                    {
                        Id = banId,
                        GameId = banId.Replace("EOS_", ""),
                        Name = playerName,
                        Reason = ban.BanReason,
                        ExpiresAt =
                            ban.BannedUntil == DateTime.MaxValue
                                ? null
                                : BanExpiry.ToTakaroUtc(ban.BannedUntil, TimeZoneInfo.Local),
                    };

                    if (banId.StartsWith("EOS_"))
                        record.EpicOnlineServicesId = banId.Replace("EOS_", "");
                    else if (banId.StartsWith("Steam_"))
                        record.SteamId = banId.Replace("Steam_", "");
                    else if (banId.StartsWith("XBL_"))
                        record.XboxLiveId = banId.Replace("XBL_", "");

                    records.Add(record);
                    seenBanIds.Add(banId);
                }
            }

            // BlockedPlayerList stores permanent platform blocks. Merge it instead of
            // falling back only when the API is unavailable, because timed bans live
            // exclusively in AdminTools.Blacklist.
            if (Platform.BlockedPlayerList.Instance != null)
            {
                foreach (
                    var blockedEntry in Platform.BlockedPlayerList.Instance.GetEntriesOrdered(
                        true,
                        false
                    )
                )
                {
                    if (blockedEntry?.PlayerData == null)
                        continue;

                    string primaryId = blockedEntry.PlayerData.PrimaryId.CombinedString;
                    string nativeId = blockedEntry.PlayerData.NativeId?.CombinedString;
                    if (
                        string.IsNullOrEmpty(primaryId)
                        || seenBanIds.Contains(primaryId)
                        || (!string.IsNullOrEmpty(nativeId) && seenBanIds.Contains(nativeId))
                    )
                        continue;

                    var record = new BanRecord
                    {
                        Id = primaryId,
                        GameId = primaryId.Replace("EOS_", ""),
                        Name = blockedEntry.PlayerData.PlayerName.Text,
                        Reason = "Blocked",
                        ExpiresAt = null,
                    };

                    if (primaryId.StartsWith("EOS_"))
                        record.EpicOnlineServicesId = primaryId.Replace("EOS_", "");

                    if (!string.IsNullOrEmpty(nativeId) && nativeId != primaryId)
                    {
                        if (nativeId.StartsWith("Steam_"))
                            record.SteamId = nativeId.Replace("Steam_", "");
                        else if (nativeId.StartsWith("XBL_"))
                            record.XboxLiveId = nativeId.Replace("XBL_", "");
                    }

                    records.Add(record);
                    seenBanIds.Add(primaryId);
                    if (!string.IsNullOrEmpty(nativeId))
                        seenBanIds.Add(nativeId);
                }
            }

            return records;
        }

        private static PlayerRecord BuildPlayerRecord(ClientInfo cInfo)
        {
            var record = new PlayerRecord
            {
                GameId = Shared.GameIdFromClientInfo(cInfo),
                Name = cInfo.playerName,
                Ip = cInfo.ip,
                Ping = cInfo.ping,
                EntityId = cInfo.entityId,
                EpicOnlineServicesId = Shared.GameIdFromClientInfo(cInfo),
                LastSeenUtc = DateTime.UtcNow,
            };

            if (cInfo.PlatformId != null && cInfo.PlatformId.CombinedString != null)
            {
                if (cInfo.PlatformId.CombinedString.StartsWith("Steam_"))
                    record.SteamId = cInfo.PlatformId.CombinedString.Replace("Steam_", "");
                else if (cInfo.PlatformId.CombinedString.StartsWith("XBL_"))
                    record.XboxLiveId = cInfo.PlatformId.CombinedString.Replace("XBL_", "");
            }

            return record;
        }

        private static void CaptureItemStacks(ItemStack[] itemStacks, List<ItemSlot> slots)
        {
            if (itemStacks == null)
                return;

            foreach (ItemStack item in itemStacks)
            {
                ItemValue itemValue = item.itemValue;
                if (itemValue == null || itemValue.Equals(ItemValue.None))
                    continue;

                ItemClass itemClass = itemValue.ItemClass;
                slots.Add(
                    new ItemSlot
                    {
                        Code = itemClass.GetItemName(),
                        Name = itemClass.GetLocalizedItemName(),
                        Description = Localization.Get($"{itemClass.GetItemName()}Desc", true),
                        Amount = item.count,
                        Quality = itemValue.Quality.ToString(),
                    }
                );
            }
        }

        private static void CaptureEquippedItems(ItemValue[] equippedItems, List<ItemSlot> slots)
        {
            if (equippedItems == null)
                return;

            foreach (ItemValue itemValue in equippedItems)
            {
                if (itemValue == null || itemValue.Equals(ItemValue.None))
                    continue;

                ItemClass itemClass = itemValue.ItemClass;
                slots.Add(
                    new ItemSlot
                    {
                        Code = itemClass.GetItemName(),
                        Name = itemClass.GetLocalizedItemName(),
                        Description = Localization.Get($"{itemClass.GetItemName()}Desc", true),
                        Amount = 1,
                        Quality = itemValue.Quality.ToString(),
                    }
                );
            }
        }

        #endregion
    }
}
