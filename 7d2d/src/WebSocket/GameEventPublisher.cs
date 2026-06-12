using System.Collections.Generic;
using UnityEngine;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Publishes game events (connect/disconnect/chat/kills/log) to Takaro.
    /// Called from game-thread event handlers; sending only enqueues onto the
    /// transport's outbound queue, so the game thread never blocks on I/O.
    /// </summary>
    public static class GameEventPublisher
    {
        public static void SendGameEvent(string type, object data)
        {
            if (data == null)
                return;

            WebSocketTransport.Instance.Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.GameEvent,
                    new Dictionary<string, object> { { "type", type }, { "data", data } }
                )
            );
        }

        public static void SendPlayerConnected(ClientInfo cInfo)
        {
            if (cInfo == null)
                return;

            SendGameEvent(
                "player-connected",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                }
            );
        }

        public static void SendPlayerDisconnected(ClientInfo cInfo)
        {
            if (cInfo == null)
                return;

            SendGameEvent(
                "player-disconnected",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                }
            );
        }

        public static void SendChatMessage(
            ClientInfo cInfo,
            EChatType type,
            int _senderId,
            string msg,
            List<int> recipientEntityIds
        )
        {
            if (cInfo == null)
                return;

            string channel;
            switch (type)
            {
                case EChatType.Global:
                    channel = "global";
                    break;
                case EChatType.Whisper:
                    channel = "whisper";
                    break;
                case EChatType.Friends:
                    channel = "friends";
                    break;
                case EChatType.Party:
                    channel = "team";
                    break;
                default:
                    channel = "unknown";
                    break;
            }

            SendGameEvent(
                "chat-message",
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                    { "msg", msg },
                    { "channel", channel },
                }
            );
        }

        public static void SendEntityKilled(
            ClientInfo killerInfo,
            string entityName,
            string entityType,
            string weapon = null
        )
        {
            if (killerInfo == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", Shared.TransformClientInfoToTakaroPlayer(killerInfo) },
                { "entity", entityType },
            };

            if (!string.IsNullOrEmpty(weapon))
            {
                eventData["weapon"] = weapon;
            }

            SendGameEvent("entity-killed", eventData);
        }

        public static void SendPlayerDeath(
            ClientInfo deadPlayerInfo,
            ClientInfo attackerInfo,
            Vector3 deathPosition
        )
        {
            if (deadPlayerInfo == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", Shared.TransformClientInfoToTakaroPlayer(deadPlayerInfo) },
                {
                    "position",
                    new Dictionary<string, object>
                    {
                        { "x", deathPosition.x },
                        { "y", deathPosition.y },
                        { "z", deathPosition.z },
                    }
                },
            };

            if (attackerInfo != null)
            {
                eventData["attacker"] = Shared.TransformClientInfoToTakaroPlayer(attackerInfo);
            }

            SendGameEvent("player-death", eventData);
        }

        public static void SendLogEvent(string logMessage)
        {
            if (string.IsNullOrEmpty(logMessage))
                return;

            SendGameEvent("log", new Dictionary<string, object> { { "msg", logMessage } });
        }
    }
}
