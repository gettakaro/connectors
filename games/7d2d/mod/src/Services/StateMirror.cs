using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.CompilerServices;
using Takaro.Interfaces;
using Takaro.Persistence;
using Takaro.WebSocket;

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
        private volatile string _inventoryUnsupportedReason;

        /// <summary>
        /// Why getPlayerInventory cannot answer on this server, or null. Set at startup when the
        /// mod is a build for the other game API (BuildTarget), or when capture first fails to
        /// bind. getPlayerInventory then fails with this text instead of returning an empty
        /// inventory, which Takaro would store as every item removed.
        /// </summary>
        public string InventoryUnsupportedReason
        {
            get
            {
                if (
                    _inventoryUnsupportedReason == null
                    && BuildTarget.Result == BuildTarget.Match.WrongApi
                )
                    _inventoryUnsupportedReason = BuildTarget.WrongBuildMessage();
                return _inventoryUnsupportedReason;
            }
        }

        public bool IsGameReady => _isGameReady;

        // World/map facts captured once on the game thread at seed time so the
        // WebSocket thread can answer getMapInfo/getMapTile without touching
        // game APIs.
        private volatile string _mapRoot;
        private int _mapSizeX;
        private int _mapSizeY;
        private int _mapSizeZ;
        private int _mapHalfExtent;
        private volatile bool _mapRenderingEnabled;
        private volatile bool _permanentDeath;
        private volatile string _worldName;

        public string MapRoot => _mapRoot;
        public int MapSizeX => _mapSizeX;
        public int MapSizeY => _mapSizeY;
        public int MapSizeZ => _mapSizeZ;

        /// <summary>Largest |x| or |z| of the world in blocks; 0 when unknown.</summary>
        public int MapHalfExtent => _mapHalfExtent;

        /// <summary>The server renders map tiles (serverconfig EnableMapRendering).</summary>
        public bool MapRenderingEnabled => _mapRenderingEnabled;

        public bool PermanentDeath => _permanentDeath;

        public string WorldName => _worldName;

        // Written on the game thread, read on the WebSocket thread. A reading lives as long as
        // the server process: Takaro only asks for online players, whom every save keeps current.
        private readonly ConcurrentDictionary<string, List<TakaroProgressionTrack>> _progression =
            new ConcurrentDictionary<string, List<TakaroProgressionTrack>>();
        private bool _progressionFailureLogged;

        // Lives ended per player, read from the registry at start and on the game thread after.
        private readonly ConcurrentDictionary<string, int> _characterLives =
            new ConcurrentDictionary<string, int>();

        // Players dead on a Permanent Death server: the game resets them when they respawn,
        // and until then each save still carries the character that ended.
        private readonly ConcurrentDictionary<string, bool> _awaitingReset =
            new ConcurrentDictionary<string, bool>();

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

        /// <summary>
        /// The whole player registry, online and offline, as the DbWriter last published it.
        /// Console commands read it on the main thread to resolve offline players, so it is a
        /// snapshot rather than a database read: the main thread never waits for the lock the
        /// DbWriter holds through seeding and checkpoints.
        /// </summary>
        public IReadOnlyList<PlayerRecord> GetRegisteredPlayers() => _registry;

        private volatile IReadOnlyList<PlayerRecord> _registry = new PlayerRecord[0];

        // DbWriter thread only: after every write that adds, renames or re-ids a player.
        private void PublishRegistry()
        {
            _registry = new List<PlayerRecord>(Database.Instance.Players.FindAll()).AsReadOnly();
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

        /// <summary>
        /// The last progression seen for a player, an empty list for a known player not
        /// saved yet (Takaro keeps what it stored), or null for an unknown player.
        /// </summary>
        public TakaroCharacter CurrentCharacter(string gameId, string name)
        {
            if (!_permanentDeath)
                return null;

            _characterLives.TryGetValue(gameId, out int life);
            return new TakaroCharacter { Id = PlayerCharacter.Id(gameId, life), Name = name };
        }

        public List<TakaroProgressionTrack> GetPlayerProgression(string gameId)
        {
            if (_progression.TryGetValue(gameId, out List<TakaroProgressionTrack> tracks))
                return tracks;

            lock (Database.Instance.SyncRoot)
            {
                return Database.Instance.Players.FindById(gameId) != null
                    ? new List<TakaroProgressionTrack>()
                    : null;
            }
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
            SeedRegistryFromGame();
            LogService.Instance.Info(
                "State mirror seeding enqueued (items, entities, locations, bans, player registry)"
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
                    _mapHalfExtent = Math.Max(
                        Math.Max(Math.Abs(min.x), Math.Abs(max.x)),
                        Math.Max(Math.Abs(min.z), Math.Abs(max.z))
                    );
                }
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn($"Could not resolve world extent: {ex.Message}");
            }

            try
            {
                _mapRenderingEnabled = GamePrefs.GetBool(EnumGamePrefs.EnableMapRendering);
                _worldName = GamePrefs.GetString(EnumGamePrefs.GameWorld);
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn($"Could not read map rendering settings: {ex.Message}");
            }

            CaptureCharacters();

            LogService.Instance.Info(
                $"World map captured (extent {_mapSizeX}x{_mapSizeY}x{_mapSizeZ}, tiles '{_mapRoot}', "
                    + $"rendering {(_mapRenderingEnabled ? "on" : "off")})"
            );
        }

        /// <summary>
        /// Adds the players the game itself remembers (players.xml) to the registry when the
        /// registry does not know them yet: players from before the registry existed, or
        /// from a database that had to be started over. Registered players are never
        /// overwritten; join, leave, spawn and the sampler keep them current.
        /// </summary>
        private void SeedRegistryFromGame()
        {
            PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
            if (playerList?.Players == null)
                return;

            var candidates = new List<PlayerRecord>();
            foreach (PersistentPlayerData data in playerList.Players.Values)
            {
                string primaryId = data?.PrimaryId?.CombinedString;
                if (primaryId == null || !primaryId.StartsWith("EOS_"))
                    continue;

                string gameId = primaryId.Substring("EOS_".Length);
                string nativeId = data.NativeId?.CombinedString;
                var record = new PlayerRecord
                {
                    GameId = gameId,
                    EpicOnlineServicesId = gameId,
                    Name = data.PlayerName?.playerName?.Text,
                    PlatformId = nativeId,
                    EntityId = data.EntityId,
                    X = data.Position.x,
                    Y = data.Position.y,
                    Z = data.Position.z,
                    LastSeenUtc = data.LastLogin.ToUniversalTime(),
                };
                if (nativeId != null && nativeId.StartsWith("Steam_"))
                    record.SteamId = nativeId.Substring("Steam_".Length);
                else if (nativeId != null && nativeId.StartsWith("XBL_"))
                    record.XboxLiveId = nativeId.Substring("XBL_".Length);
                candidates.Add(record);
            }

            DbWriter.Instance.Enqueue(() =>
            {
                int added = 0;
                foreach (PlayerRecord record in candidates)
                {
                    if (Database.Instance.Players.FindById(record.GameId) != null)
                        continue;
                    Database.Instance.Players.Insert(record);
                    added++;
                }
                if (added > 0)
                    LogService.Instance.Info(
                        $"Player registry: added {added} player(s) the game remembers from players.xml"
                    );
                PublishRegistry();
            });
        }

        public void UpsertPlayerOnline(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null)
                return;

            PlayerRecord record = Shared.BuildPlayerRecord(cInfo);
            record.Online = true;
            _characterLives.TryGetValue(record.GameId, out int life);
            record.CharacterLife = life;
            record.AwaitingCharacterReset = _awaitingReset.ContainsKey(record.GameId);

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

            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Players.Upsert(record);
                PublishRegistry();
            });
        }

        private void CaptureCharacters()
        {
            try
            {
                _permanentDeath =
                    GameStats.GetInt(EnumGameStats.DeathPenalty)
                    == (int)EnumDeathPenalty.Permadeath;
            }
            catch (Exception ex)
            {
                LogService.Instance.Warn($"Could not read the death penalty: {ex.Message}");
            }

            if (_permanentDeath)
                LogService.Instance.Info("Permanent Death is on: reporting player characters");

            DbWriter.Instance.Enqueue(() =>
            {
                foreach (PlayerRecord record in Database.Instance.Players.FindAll())
                {
                    _characterLives[record.GameId] = record.CharacterLife;
                    if (record.AwaitingCharacterReset)
                        _awaitingReset[record.GameId] = true;
                }
            });
        }

        /// <summary>
        /// A death on a Permanent Death server: the next character starts here. Its tracks
        /// stay empty until the respawn resets the player and the client saves again.
        /// </summary>
        public void EndCharacter(string gameId)
        {
            if (!_permanentDeath)
                return;

            int life = _characterLives.AddOrUpdate(gameId, 1, (_, ended) => ended + 1);
            _awaitingReset[gameId] = true;
            _progression.TryRemove(gameId, out _);
            DbWriter.Instance.Enqueue(() =>
            {
                Database.Instance.Inventories.Delete(gameId);
                PlayerRecord record = Database.Instance.Players.FindById(gameId);
                if (record == null)
                    return;
                record.CharacterLife = life;
                record.AwaitingCharacterReset = true;
                Database.Instance.Players.Update(record);
            });
        }

        public void CharacterRespawned(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null)
                return;

            string gameId = Shared.GameIdFromClientInfo(cInfo);
            if (!_awaitingReset.TryRemove(gameId, out _))
                return;

            DbWriter.Instance.Enqueue(() =>
            {
                PlayerRecord record = Database.Instance.Players.FindById(gameId);
                if (record == null)
                    return;
                record.AwaitingCharacterReset = false;
                Database.Instance.Players.Update(record);
            });
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

        /// <summary>
        /// Caches a player's tracks from the PlayerDataFile the client just pushed, at login and
        /// on each of its saves. The live entity is no use here: the client only sends it new
        /// values when its stats flag is set, so its experience goes stale between saves.
        /// </summary>
        public void UpsertProgression(ClientInfo cInfo)
        {
            if (cInfo?.CrossplatformId == null || cInfo.latestPlayerData == null)
                return;
            if (_awaitingReset.ContainsKey(Shared.GameIdFromClientInfo(cInfo)))
                return;

            try
            {
                byte[] blob = cInfo.latestPlayerData.progressionData.ToArray();
                if (blob.Length == 0)
                    return;

                ProgressionBlob saved = ProgressionBlob.Parse(blob);
                // Parameterless, so it only serves the level costs and touches no player.
                var curve = new Progression();
                Func<int, int> costOfLevel = level =>
                    curve.getExpForLevel(
                        UnityEngine.Mathf.Clamp(level + 1, 0, Progression.ClampExpCostAtLevel)
                    );

                _progression[Shared.GameIdFromClientInfo(cInfo)] = PlayerProgression.Build(
                    saved.Level,
                    PlayerProgression.TotalExperience(
                        saved.Level,
                        saved.ExpToNextLevel,
                        costOfLevel
                    ),
                    PlayerProgression.LevelProgress(saved.ExpToNextLevel, costOfLevel(saved.Level)),
                    ProgressionTracks(saved)
                );
            }
            catch (Exception ex)
            {
                if (!_progressionFailureLogged)
                {
                    _progressionFailureLogged = true;
                    LogService.Instance.Warn($"Could not read player progression: {ex.Message}");
                }
            }
        }

        /// <summary>
        /// Attributes, perks and crafting skills, at level 0 too: a track missing from a
        /// reading means "unknown" to Takaro, so a perk reset would otherwise never show and
        /// a first perk point would be a silent baseline. Skills are left out because the game
        /// reports them at their maximum, books because they are collected, not levelled.
        /// </summary>
        private static IEnumerable<TakaroProgressionTrack> ProgressionTracks(ProgressionBlob saved)
        {
            var known = new List<KeyValuePair<ProgressionClass, int>>();
            foreach (KeyValuePair<string, int> value in saved.Values)
            {
                if (
                    Progression.ProgressionClasses.TryGetValue(value.Key, out ProgressionClass kind)
                    && !kind.Hidden
                    && kind.MaxLevel > 0
                    && (kind.IsAttribute || kind.IsPerk || kind.IsCrafting)
                )
                    known.Add(new KeyValuePair<ProgressionClass, int>(kind, value.Value));
            }

            return known
                .OrderBy(p =>
                    p.Key.IsAttribute ? 0
                    : p.Key.IsPerk ? 1
                    : 2
                )
                .ThenBy(p => p.Key.Name, StringComparer.Ordinal)
                .Select(p => new TakaroProgressionTrack
                {
                    Key = p.Key.Name,
                    Name = Shared.LocalizedOrNull(
                        p.Key.NameKey,
                        Localization.Get(p.Key.NameKey, true)
                    ),
                    Level = p.Value,
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
            if (InventoryUnsupportedReason != null)
                return;
            if (
                cInfo?.CrossplatformId != null
                && _awaitingReset.ContainsKey(Shared.GameIdFromClientInfo(cInfo))
            )
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
                _inventoryUnsupportedReason =
                    $"Inventory capture is not supported on this game build ({ex.Message}). This mod was built for "
                    + $"7 Days to Die {BuildTarget.Display(BuildTarget.Built)} and the server runs "
                    + $"{BuildTarget.Display(BuildTarget.Running)}; install the zip for your build from {BuildTarget.DownloadPage}.";
                LogService.Instance.Error(
                    _inventoryUnsupportedReason
                        + " getPlayerInventory now fails until that is fixed."
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
        /// Used by the entity catalogue only: entity-killed keeps the class name
        /// for parity with the legacy integration (see Shared.EntityKillName).
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
                            // Takaro reads position as the centre of the area; the
                            // code above keeps the min corner so codes stay stable.
                            X = instance.boundingBoxPosition.x + instance.boundingBoxSize.x / 2,
                            Y = instance.boundingBoxPosition.y + instance.boundingBoxSize.y / 2,
                            Z = instance.boundingBoxPosition.z + instance.boundingBoxSize.z / 2,
                            SizeX = instance.boundingBoxSize.x,
                            SizeY = instance.boundingBoxSize.y,
                            SizeZ = instance.boundingBoxSize.z,
                            PrefabName = prefabName,
                            Rotation = instance.rotation,
                            PositionAnchor = "centre",
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
        public static PersistentPlayerData FindPersistentPlayer(
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
            PersistentPlayerList playerList = GameManager.Instance.GetPersistentPlayerList();
            var blacklist = new List<BanIdentity.BanSource>();
            var blocked = new List<BanIdentity.BanSource>();

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
                    string knownName = playerData?.PlayerName.playerName.Text;
                    blacklist.Add(
                        new BanIdentity.BanSource
                        {
                            BanId = banId,
                            CrossplatformId = playerData?.PrimaryId?.CombinedString,
                            NativeId = playerData?.NativeId?.CombinedString,
                            Name = string.IsNullOrEmpty(knownName) ? ban.Name : knownName,
                            Reason = ban.BanReason,
                            ExpiresAt =
                                ban.BannedUntil == DateTime.MaxValue
                                    ? null
                                    : BanExpiry.ToTakaroUtc(ban.BannedUntil, TimeZoneInfo.Local),
                        }
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

                    blocked.Add(
                        new BanIdentity.BanSource
                        {
                            BanId = primaryId,
                            CrossplatformId = primaryId,
                            NativeId = blockedEntry.PlayerData.NativeId?.CombinedString,
                            Name = blockedEntry.PlayerData.PlayerName.Text,
                            Reason = "Blocked",
                        }
                    );
                }
            }

            return BanIdentity.Capture(
                blacklist,
                blocked,
                skippedId =>
                    LogService.Instance.Debug(
                        $"Ban entry {skippedId} has no EOS id or no name; not sent"
                    )
            );
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
