using System;
using System.Collections.Generic;
using System.IO;
using HarmonyLib;
using Takaro.Config;
using Takaro.Interfaces;
using Takaro.Services;
using Takaro.WebSocket;
using UnityEngine;

namespace Takaro
{
    public class API : IModApi
    {
        private readonly List<IService> Services = new List<IService>
        {
            LogService.Instance
        };

        public const string ModPrefix = "Takaro";
        public static readonly string BasePath = Directory.GetCurrentDirectory() + "/Takaro";
        private WebSocketClient _webSocketClient;

        public void InitMod(Mod mod)
        {
            ServiceRegistry.InitServices();
            
            LogService.Instance.Info("Initializing mod");
            
            if (!Directory.Exists(BasePath))
                Directory.CreateDirectory(BasePath);

            // Initialize config
            ConfigManager.Instance.LoadConfig();

            // Register event handlers
            ModEvents.GameStartDone.RegisterHandler(GameStartDoneHandler);
            ModEvents.GameStartDone.RegisterHandler(GameAwake);
            ModEvents.GameShutdown.RegisterHandler(GameShutdown);
            ModEvents.SavePlayerData.RegisterHandler(SavePlayerData);
            ModEvents.PlayerSpawnedInWorld.RegisterHandler(PlayerSpawnedInWorld);
            ModEvents.PlayerDisconnected.RegisterHandler(PlayerDisconnected);
            ModEvents.PlayerLogin.RegisterHandler(PlayerLogin);
            ModEvents.EntityKilled.RegisterHandler(EntityKilled);
            ModEvents.GameMessage.RegisterHandler(GameMessage);

            // Register Unity log handler for capturing server logs
            Application.logMessageReceived += HandleLogMessage;

            LogService.Instance.Info("Mod initialized successfully");
        }
        
        private static void GameStartDoneHandler(ref ModEvents.SGameStartDoneData data)
        {
            var harmony = new Harmony("com.takaro.patch");
            harmony.PatchAll();
        }

        private ModEvents.EModEventResult GameMessage(ref ModEvents.SGameMessageData data)
        {
            return ModEvents.EModEventResult.Continue;
        }

        private void GameAwake(ref ModEvents.SGameStartDoneData data)
        {
            // Initialize WebSocket client
            _webSocketClient = WebSocketClient.Instance;
            _webSocketClient.Initialize();
        }

        private void GameShutdown(ref ModEvents.SGameShutdownData data)
        {
            LogService.Instance.Info("Game shutting down");

            // Unregister Unity log handler
            Application.logMessageReceived -= HandleLogMessage;

            // Shutdown WebSocket client
            _webSocketClient?.Shutdown();
        }

        private void PlayerDisconnected(ref ModEvents.SPlayerDisconnectedData data)
        {
            if (data.ClientInfo != null && !data.GameShuttingDown)
            {
                LogService.Instance.Debug($"Player disconnected: {data.ClientInfo.playerName} ({data.ClientInfo.PlatformId})");
                _webSocketClient?.SendPlayerDisconnected(data.ClientInfo);
            }
        }

        public void EntityKilled(ref ModEvents.SEntityKilledData data)
        {
            if (data.KilledEntitiy != null)
            {
                // Handle player death events
                if (data.KilledEntitiy.entityType == EntityType.Player)
                {
                    ClientInfo killedPlayerInfo = ConsoleHelper.ParseParamIdOrName(
                        data.KilledEntitiy.entityId.ToString()
                    );
                    if (killedPlayerInfo != null)
                    {
                        // Get killer information
                        ClientInfo attackerInfo = null;
                        if (data.KillingEntity != null && data.KillingEntity.entityType == EntityType.Player)
                        {
                            attackerInfo = ConsoleHelper.ParseParamIdOrName(
                                data.KillingEntity.entityId.ToString()
                            );
                        }

                        Vector3 deathPosition = data.KilledEntitiy.position;
                        LogService.Instance.Debug($"Player death: {killedPlayerInfo.playerName} died at {deathPosition}");
                        
                        _webSocketClient?.SendPlayerDeath(killedPlayerInfo, attackerInfo, deathPosition);
                    }
                }
                // Handle entity kill events (player killing something else)
                else if (data.KillingEntity != null && data.KillingEntity.entityType == EntityType.Player)
                {
                    ClientInfo killerInfo = ConsoleHelper.ParseParamIdOrName(
                        data.KillingEntity.entityId.ToString()
                    );
                    if (killerInfo == null)
                        return;
                    EntityAlive ea = data.KilledEntitiy as EntityAlive;
                    if (ea == null)
                        return;

                    string entityType = "unknown";
                    if (data.KilledEntitiy.entityType == EntityType.Zombie)
                    {
                        entityType = "zombie";
                        LogService.Instance.Debug(
                            $"Entity killed: {killerInfo.playerName} ({killerInfo.PlatformId}) killed zombie {ea.EntityName}"
                        );
                    }
                    else if (data.KilledEntitiy.entityType == EntityType.Animal)
                    {
                        entityType = "animal";
                        LogService.Instance.Debug(
                            $"Entity killed: {killerInfo.playerName} ({killerInfo.PlatformId}) killed animal {ea.EntityName}"
                        );
                    }
                    else
                    {
                        entityType = data.KilledEntitiy.entityType.ToString().ToLower();
                    }

                    // Try to get weapon information from player's held item
                    string weapon = null;
                    try
                    {
                        EntityPlayer playerEntity = data.KillingEntity as EntityPlayer;
                        if (playerEntity != null && playerEntity.inventory != null)
                        {
                            ItemValue heldItemValue = playerEntity.inventory.holdingItemItemValue;
                            if (heldItemValue != null && !heldItemValue.IsEmpty())
                            {
                                ItemClass itemClass = heldItemValue.ItemClass;
                                weapon = itemClass?.GetLocalizedItemName() ?? itemClass?.GetItemName();
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        LogService.Instance.Warn($"Could not get weapon info: {ex.Message}");
                    }

                    _webSocketClient?.SendEntityKilled(killerInfo, ea.EntityName, entityType, weapon);
                }
            }
        }

        private void PlayerSpawnedInWorld(ref ModEvents.SPlayerSpawnedInWorldData data)
        {
            if (data.ClientInfo == null)
                return;

            if (
                data.RespawnType == RespawnType.JoinMultiplayer
                || data.RespawnType == RespawnType.EnterMultiplayer
            )
            {
                LogService.Instance.Debug($"Player connected: {data.ClientInfo.playerName} ({data.ClientInfo.PlatformId})");
                _webSocketClient?.SendPlayerConnected(data.ClientInfo);
            }
        }

        private void SavePlayerData(ref ModEvents.SSavePlayerDataData data)
        {
            // Can be used to track player stats if needed
        }

        private ModEvents.EModEventResult PlayerLogin(ref ModEvents.SPlayerLoginData data)
        {
            return ModEvents.EModEventResult.Continue;
        }

        private void HandleLogMessage(string logString, string stackTrace, LogType type)
        {
            // Filter log messages to only send relevant ones to Takaro
            // Avoid infinite loops by not sending our own Takaro log messages
            if (string.IsNullOrEmpty(logString) || logString.Contains($"[{ModPrefix}]"))
                return;

            // Only send Error and Warning level messages to reduce noise
            if (type == LogType.Error || type == LogType.Warning)
            {
                string formattedMessage = $"[{type}] {logString}";
                if (!string.IsNullOrEmpty(stackTrace) && type == LogType.Error)
                {
                    formattedMessage += $"\nStack Trace: {stackTrace}";
                }

                _webSocketClient?.SendLogEvent(formattedMessage);
            }
        }
        
        [HarmonyPatch(typeof(NetPackageChat), "ProcessPackage")]
        public class NetPackageChat_ProcessPackage_Patch
        {
            private static bool Prefix(NetPackageChat __instance, World _world, GameManager _callbacks, string ___msg)
            {
                ClientInfo cInfo = SingletonMonoBehaviour<ConnectionManager>.Instance.Clients.ForEntityId(__instance.senderEntityId);
                if (cInfo != null)
                {
                    LogService.Instance.Debug($"Chat message: {cInfo.playerName}: {___msg}");
                    WebSocketClient.Instance?.SendChatMessage(cInfo, __instance.chatType, __instance.senderEntityId, ___msg, __instance.recipientEntityIds);
                }
                return true;
            }
        }
    }
}
