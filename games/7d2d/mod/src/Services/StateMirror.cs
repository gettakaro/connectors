using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;
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
        private volatile bool _inventoryCaptureUnsupported;

        public bool IsGameReady => _isGameReady;

        // World/map facts captured once on the game thread at seed time so the
        // WebSocket thread can answer getMapInfo/getMapTile without touching
        // game APIs.
        private volatile string _mapRoot;
        private int _mapSizeX;
        private int _mapSizeY;
        private int _mapSizeZ;

        public string MapRoot => _mapRoot;
        public int MapSizeX => _mapSizeX;
        public int MapSizeY => _mapSizeY;
        public int MapSizeZ => _mapSizeZ;

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
            SeedWorldMap();
            RefreshBans();
            LogService.Instance.Info(
                "State mirror seeding enqueued (items, entities, locations, bans)"
            );
        }

        /// <summary>
        /// Captures the world extent and the web-map tile-cache root. Both are
        /// game-thread-only reads, so they happen here rather than per request.
        /// </summary>
        private void SeedWorldMap()
        {
            try
            {
                _mapRoot = System.IO.Path.Combine(GameIO.GetSaveGameDir(), "map");
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn($"Could not resolve map tile cache: {ex.Message}");
                _mapRoot = null;
            }

            try
            {
                World world = GameManager.Instance?.World;
                Vector3i min;
                Vector3i max;
                if (world != null && world.GetWorldExtent(out min, out max))
                {
                    _mapSizeX = Math.Abs(max.x - min.x);
                    _mapSizeY = Math.Abs(max.y - min.y);
                    _mapSizeZ = Math.Abs(max.z - min.z);
                }
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn($"Could not resolve world extent: {ex.Message}");
            }

            LogService.Instance.Info(
                $"World map captured (extent {_mapSizeX}x{_mapSizeY}x{_mapSizeZ}, tiles '{_mapRoot}')"
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
            if (_inventoryCaptureUnsupported)
                return;

            // A game build that reshapes PlayerDataFile (V3.3.0 serialises the
            // inventory into MemoryStreams) fails here when Mono binds the fields.
            // Skip the inventory mirror then, so player events still go out.
            try
            {
                CaptureInventory(cInfo);
            }
            catch (Exception ex)
                when (ex is MissingFieldException
                    || ex is MissingMethodException
                    || ex is TypeLoadException
                )
            {
                _inventoryCaptureUnsupported = true;
                LogService.Instance.Warn(
                    $"Inventory capture is not supported on this game build ({ex.Message}); getPlayerInventory will be empty"
                );
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private void CaptureInventory(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null || cInfo.latestPlayerData == null)
                return;

            var slots = new List<ItemSlot>();
#if SEVEND2D_V3_3
            PlayerDataFile data = cInfo.latestPlayerData;
            CaptureItemStacks(ReadItemGrid(data.inventoryData), slots);
            CaptureItemStacks(ReadItemGrid(data.bagData), slots);
            CaptureItemStacks(ReadItemGrid(data.equipmentData), slots);
#else
            CaptureItemStacks(cInfo.latestPlayerData.inventory, slots);
            CaptureItemStacks(cInfo.latestPlayerData.bag?.GetSlots(), slots);
            CaptureEquippedItems(cInfo.latestPlayerData.equipment?.GetItems(), slots);
#endif

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

                // Entries the game never names for players (imposters, test
                // blocks, perk-tier helpers, zombie hands: 275 of 2600 on
                // V3.2.0) are internal and do not belong in the catalogue.
                // giveItem resolves codes against the game itself, so they can
                // still be given by code.
                string name = Shared.LocalizedOrNull(
                    item.GetItemName(),
                    item.GetLocalizedItemName()
                );
                if (name == null)
                    continue;

                records.Add(
                    new ItemRecord
                    {
                        Code = item.GetItemName(),
                        Name = name,
                        Description = Shared.ItemDescription(item.GetItemName()),
                    }
                );
            }

            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Items.DeleteAll();
                Database.Instance.Items.InsertBulk(records);
            });
        }

        /// <summary>
        /// Player-facing name for an entity class ("zombieBoe" -> "Boe"), falling
        /// back to the class name when the game has no localisation for it.
        /// Shared by the entity catalogue and entity-killed so they agree.
        /// </summary>
        public static string EntityDisplayName(string entityClassName)
        {
            if (string.IsNullOrEmpty(entityClassName))
                return entityClassName;

            string localizedName = Localization.Get(entityClassName, true);
            return string.IsNullOrEmpty(localizedName) ? entityClassName : localizedName;
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

                string localizedName = EntityDisplayName(code);

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

        // Persistent players are keyed by their EOS primary id, so a Steam_/XBL_ ban
        // entry is translated by matching the player's native id. Unknown players
        // stay untranslated and are skipped by BanIdentity.
        private static PersistentPlayerData FindPersistentPlayer(
            PersistentPlayerList playerList,
            PlatformUserIdentifierAbs banned
        )
        {
            PersistentPlayerData byPrimaryId = playerList?.GetPlayerData(banned);
            if (byPrimaryId != null || playerList == null)
                return byPrimaryId;

            foreach (PersistentPlayerData candidate in playerList.Players.Values)
            {
                if (candidate.NativeId?.CombinedString == banned.CombinedString)
                    return candidate;
            }
            return null;
        }

        private static List<BanRecord> CaptureBans()
        {
            var records = new List<BanRecord>();
            var seenGameIds = new HashSet<string>();
            PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();

            void Add(BanRecord record, string banId)
            {
                if (record == null)
                {
                    LogService.Instance.Debug($"Ban entry {banId} has no EOS id; not sent");
                    return;
                }
                if (seenGameIds.Add(record.GameId))
                    records.Add(record);
            }

            // AdminTools.Blacklist stores timed bans and preserves reason/expiry metadata.
            if (GameManager.Instance?.adminTools?.Blacklist != null)
            {
                foreach (var ban in GameManager.Instance.adminTools.Blacklist.GetBanned())
                {
                    if (ban.UserIdentifier == null)
                        continue;

                    string banId = ban.UserIdentifier.CombinedString;
                    if (string.IsNullOrEmpty(banId))
                        continue;

                    PersistentPlayerData playerData = FindPersistentPlayer(
                        playerList,
                        ban.UserIdentifier
                    );
                    Add(
                        BanIdentity.ToRecord(
                            banId,
                            playerData?.PrimaryId?.CombinedString,
                            playerData?.NativeId?.CombinedString,
                            playerData?.PlayerName.playerName.Text,
                            ban.BanReason,
                            ban.BannedUntil == DateTime.MaxValue
                                ? null
                                : BanExpiry.ToTakaroUtc(ban.BannedUntil, TimeZoneInfo.Local)
                        ),
                        banId
                    );
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
                    if (string.IsNullOrEmpty(primaryId))
                        continue;

                    Add(
                        BanIdentity.ToRecord(
                            primaryId,
                            primaryId,
                            blockedEntry.PlayerData.NativeId?.CombinedString,
                            blockedEntry.PlayerData.PlayerName.Text,
                            "Blocked",
                            null
                        ),
                        primaryId
                    );
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

#if SEVEND2D_V3_3
        /// <summary>
        /// V3.3.0 keeps the toolbelt, backpack and equipment in PlayerDataFile as
        /// serialised blobs. Each starts with a container version byte followed by
        /// an ItemStackGrid (Inventory/Bag/Equipment.Write), so the grid is read on
        /// its own, without binding it to a live entity.
        /// </summary>
        private static ItemStack[] ReadItemGrid(System.IO.MemoryStream blob)
        {
            ItemStack[] items = null;
            StreamUtils.FromBlob(
                blob,
                reader =>
                {
                    reader.ReadByte();
                    items = ItemStackGrid.Read(reader, StreamModeRead.Persistency).items;
                }
            );
            return items;
        }
#endif

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
                        Description = Shared.ItemDescription(itemClass.GetItemName()),
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
                        Description = Shared.ItemDescription(itemClass.GetItemName()),
                        Amount = 1,
                        Quality = itemValue.Quality.ToString(),
                    }
                );
            }
        }

        #endregion
    }
}
