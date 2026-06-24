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

            // Register Unity log handler for capturing server logs
            Application.logMessageReceived += HandleLogMessage;

            LogService.Instance.Info("Mod initialized successfully");
        }

        private static void GameStartDone(ref ModEvents.SGameStartDoneData data)
        {
            var harmony = new Harmony("com.takaro.patch");
            harmony.PatchAll();

            // Seed the mirror from game truth before the WebSocket connects, so
            // requests can never observe a cold mirror.
            StateMirror.Instance.SeedOnGameStart();
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

            // Unregister Unity log handler
            Application.logMessageReceived -= HandleLogMessage;

            MainThreadDispatcher.Instance.Shutdown();
            WebSocketTransport.Instance.Shutdown();
            ServiceRegistry.DestroyServices();
        }

        private static void PlayerDisconnected(ref ModEvents.SPlayerDisconnectedData data)
        {
            if (data.ClientInfo == null)
                return;

            StateMirror.Instance.MarkOffline(data.ClientInfo);

            if (!data.GameShuttingDown)
            {
                LogService.Instance.Debug(
                    $"Player disconnected: {data.ClientInfo.playerName} ({data.ClientInfo.PlatformId})"
                );
                GameEventPublisher.SendPlayerDisconnected(data.ClientInfo);
            }
        }

        public void EntityKilled(ref ModEvents.SEntityKilledData data)
        {
            if (data.KilledEntitiy == null)
                return;

            // Handle player death events
            if (data.KilledEntitiy.entityType == EntityType.Player)
            {
                ClientInfo killedPlayerInfo = ConsoleHelper.ParseParamIdOrName(
                    data.KilledEntitiy.entityId.ToString()
                );
                if (killedPlayerInfo != null)
                {
                    ClientInfo attackerInfo = null;
                    if (
                        data.KillingEntity != null
                        && data.KillingEntity.entityType == EntityType.Player
                    )
                    {
                        attackerInfo = ConsoleHelper.ParseParamIdOrName(
                            data.KillingEntity.entityId.ToString()
                        );
                    }

                    Vector3 deathPosition = data.KilledEntitiy.position;
                    LogService.Instance.Debug(
                        $"Player death: {killedPlayerInfo.playerName} died at {deathPosition}"
                    );

                    GameEventPublisher.SendPlayerDeath(
                        killedPlayerInfo,
                        attackerInfo,
                        deathPosition
                    );
                }
            }
            // Handle entity kill events (player killing something else)
            else if (
                data.KillingEntity != null
                && data.KillingEntity.entityType == EntityType.Player
            )
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

                GameEventPublisher.SendEntityKilled(killerInfo, ea.EntityName, entityType, weapon);
            }
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

        private static void HandleLogMessage(string logString, string stackTrace, LogType type)
        {
            // Forward raw server log lines to Takaro while avoiding feedback loops from
            // the connector's own LogService output.
            if (string.IsNullOrEmpty(logString) || logString.Contains($"[{ModPrefix}]"))
                return;

            GameEventPublisher.SendLogEvent(logString);
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
