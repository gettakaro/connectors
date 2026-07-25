using System;
using System.IO;
using HarmonyLib;
using Takaro.Config;
using Takaro.Services;
using Takaro.WebSocket;
using UnityEngine;

namespace Takaro
{
    public class API : IModApi
    {
        public const string ModPrefix = "Takaro";
        public static readonly string BasePath = Directory.GetCurrentDirectory() + "/Takaro";

        public void InitMod(Mod mod)
        {
            if (!Directory.Exists(BasePath))
                Directory.CreateDirectory(BasePath);

            ServiceRegistry.InitServices();

            LogService.Instance.Info("Initializing mod");

            // Initialize config
            ConfigManager.Instance.LoadConfig();

            // Register event handlers
            ModEvents.GameStartDone.RegisterHandler(GameStartDone);
            ModEvents.GameUpdate.RegisterHandler(GameUpdate);
            ModEvents.GameShutdown.RegisterHandler(GameShutdown);
            ModEvents.SavePlayerData.RegisterHandler(SavePlayerData);
            ModEvents.PlayerSpawnedInWorld.RegisterHandler(PlayerSpawnedInWorld);
            ModEvents.PlayerDisconnected.RegisterHandler(PlayerDisconnected);
            ModEvents.EntityKilled.RegisterHandler(EntityKilled);
            ModEvents.GameMessage.RegisterHandler(GameMessage);

            // Capture the dedicated server's native Log.Out stream as well as Unity logs.
            Log.LogCallbacksExtended += HandleNativeLogMessage;

            LogService.Instance.Info("Mod initialized successfully");
        }

        private static void GameStartDone(ref ModEvents.SGameStartDoneData data)
        {
            var harmony = new Harmony("com.takaro.patch");
            harmony.PatchAll();

            // Seed the mirror from game truth before the WebSocket connects, so
            // requests can never observe a cold mirror.
            StateMirror.Instance.SeedOnGameStart();
            DbWriter.Instance.Flush(TimeSpan.FromSeconds(30));
            StateMirror.Instance.MarkGameReady();
            WebSocketTransport.Instance.Initialize();
        }

        private static void GameUpdate(ref ModEvents.SGameUpdateData data)
        {
            MainThreadDispatcher.Instance.OnGameUpdate(ref data);
            PositionSampler.Instance.OnGameUpdate(ref data);
        }

        private static void GameShutdown(ref ModEvents.SGameShutdownData data)
        {
            LogService.Instance.Info("Game shutting down");
            StateMirror.Instance.MarkGameStopping();

            ModEvents.GameMessage.UnregisterHandler(GameMessage);
            Log.LogCallbacksExtended -= HandleNativeLogMessage;

            MainThreadDispatcher.Instance.Shutdown();
            WebSocketTransport.Instance.Shutdown();
            ServiceRegistry.DestroyServices();
        }

        private static void PlayerDisconnected(ref ModEvents.SPlayerDisconnectedData data)
        {
            if (data.ClientInfo == null)
                return;

            TakaroPlayer player = Shared.TransformClientInfoToTakaroPlayerIdentity(data.ClientInfo);
            StateMirror.Instance.MarkOffline(data.ClientInfo);

            if (!data.GameShuttingDown && player != null)
            {
                LogService.Instance.Debug(
                    $"Player disconnected: {data.ClientInfo.playerName} ({data.ClientInfo.PlatformId})"
                );
                GameEventPublisher.SendPlayerDisconnected(player);
            }
        }

        public void EntityKilled(ref ModEvents.SEntityKilledData data)
        {
            if (data.KilledEntitiy == null)
                return;

            if (data.KilledEntitiy.entityType == EntityType.Player)
                return;

            // Player deaths use GameMessage. EntityKilled remains the first-party
            // surface for a player killing a non-player living entity.
            if (data.KillingEntity != null && data.KillingEntity.entityType == EntityType.Player)
            {
                ClientInfo killerInfo = ConsoleHelper.ParseParamIdOrName(
                    data.KillingEntity.entityId.ToString()
                );
                if (killerInfo == null)
                    return;
                EntityAlive ea = data.KilledEntitiy as EntityAlive;
                if (ea == null)
                    return;

                string entityType;
                if (data.KilledEntitiy.entityType == EntityType.Zombie)
                    entityType = "zombie";
                else if (data.KilledEntitiy.entityType == EntityType.Animal)
                    entityType = "animal";
                else
                    entityType = data.KilledEntitiy.entityType.ToString().ToLower();

                // Try to get weapon information from player's held item
                string weapon = null;
                EntityPlayer playerEntity = data.KillingEntity as EntityPlayer;
                if (playerEntity?.inventory != null)
                {
                    ItemValue heldItemValue = playerEntity.inventory.holdingItemItemValue;
                    if (heldItemValue != null && !heldItemValue.IsEmpty())
                    {
                        ItemClass itemClass = heldItemValue.ItemClass;
                        weapon = itemClass?.GetLocalizedItemName() ?? itemClass?.GetItemName();
                    }
                }

                TakaroPlayer killer = Shared.TransformClientInfoToTakaroPlayerIdentity(killerInfo);
                GameEventPublisher.SendEntityKilled(killer, ea.EntityName, entityType, weapon);
            }
        }

        private static ModEvents.EModEventResult GameMessage(ref ModEvents.SGameMessageData data)
        {
            if (data.MessageType != EnumGameMessages.EntityWasKilled)
                return ModEvents.EModEventResult.Continue;

            ClientInfo victimInfo = data.ClientInfo;
            EntityPlayer victimEntity = null;
            if (victimInfo != null)
            {
                GameManager.Instance.World.Players.dict.TryGetValue(
                    victimInfo.entityId,
                    out victimEntity
                );
            }
            else
            {
                foreach (EntityPlayer candidate in GameManager.Instance.World.Players.dict.Values)
                {
                    if (
                        candidate == null
                        || !string.Equals(
                            candidate.PlayerDisplayName,
                            data.MainName,
                            StringComparison.Ordinal
                        )
                    )
                        continue;

                    if (victimEntity != null)
                    {
                        LogService.Instance.Warn(
                            $"Skipped ambiguous player-death event for '{data.MainName}'"
                        );
                        return ModEvents.EModEventResult.Continue;
                    }
                    victimEntity = candidate;
                }

                if (victimEntity != null)
                    victimInfo = ConnectionManager.Instance.Clients.ForEntityId(
                        victimEntity.entityId
                    );
            }

            TakaroPlayer victim = Shared.TransformClientInfoToTakaroPlayerIdentity(victimInfo);
            if (victim == null)
            {
                LogService.Instance.Warn("Skipped player-death event without stable identity");
                return ModEvents.EModEventResult.Continue;
            }

            Vector3 deathPosition;
            if (victimEntity != null)
            {
                deathPosition = victimEntity.GetPosition();
            }
            else if (victimInfo.latestPlayerData != null)
            {
                deathPosition = victimInfo.latestPlayerData.ecd.pos;
            }
            else
            {
                LogService.Instance.Warn("Skipped player-death event without a position");
                return ModEvents.EModEventResult.Continue;
            }

            GameEventPublisher.SendPlayerDeath(victim, null, deathPosition);
            return ModEvents.EModEventResult.Continue;
        }

        private static void PlayerSpawnedInWorld(ref ModEvents.SPlayerSpawnedInWorldData data)
        {
            if (data.ClientInfo == null)
                return;

            // Refresh the mirror on every spawn type (join, respawn, teleport)
            StateMirror.Instance.UpsertPlayerOnline(data.ClientInfo);

            if (
                data.RespawnType == RespawnType.JoinMultiplayer
                || data.RespawnType == RespawnType.EnterMultiplayer
            )
            {
                // Seed the inventory mirror from the player data received at login
                StateMirror.Instance.UpsertInventory(data.ClientInfo);

                LogService.Instance.Debug(
                    $"Player connected: {data.ClientInfo.playerName} ({data.ClientInfo.PlatformId})"
                );
                GameEventPublisher.SendPlayerConnected(data.ClientInfo);
            }
        }

        private static void SavePlayerData(ref ModEvents.SSavePlayerDataData data)
        {
            // The client just pushed a fresh PlayerDataFile — mirror the inventory
            StateMirror.Instance.UpsertInventory(data.ClientInfo);
        }

        private static void HandleNativeLogMessage(
            string formattedMessage,
            string plainMessage,
            string trace,
            LogType type,
            DateTime timestamp,
            long uptime
        )
        {
            // Forward raw server log lines to Takaro while avoiding feedback loops from
            // the connector's own LogService output.
            if (
                string.IsNullOrEmpty(plainMessage)
                || plainMessage.Contains($"[{ModPrefix}]")
                || ServerMessageEchoGuard.Instance.ShouldSuppress(plainMessage)
            )
                return;

            GameEventPublisher.SendLogEvent(plainMessage);
        }

        [HarmonyPatch(typeof(NetPackageChat), "ProcessPackage")]
        public class NetPackageChat_ProcessPackage_Patch
        {
            private static bool Prefix(
                NetPackageChat __instance,
                World _world,
                GameManager _callbacks,
                string ___msg
            )
            {
                ClientInfo cInfo =
                    SingletonMonoBehaviour<ConnectionManager>.Instance.Clients.ForEntityId(
                        __instance.senderEntityId
                    );
                if (cInfo != null)
                {
                    LogService.Instance.Debug($"Chat message: {cInfo.playerName}: {___msg}");
                    GameEventPublisher.SendChatMessage(
                        cInfo,
                        __instance.chatType,
                        __instance.senderEntityId,
                        ___msg,
                        __instance.recipientEntityIds
                    );
                }
                return true;
            }
        }
    }
}
