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

        public static void SendPlayerDisconnected(TakaroPlayer player)
        {
            if (player == null)
                return;

            SendGameEvent(
                "player-disconnected",
                new Dictionary<string, object> { { "player", player } }
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
            TakaroPlayer killer,
            string entityName,
            string entityType,
            string weapon = null
        )
        {
            if (killer == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", killer },
                { "entity", entityType },
                { "weapon", string.IsNullOrEmpty(weapon) ? "unknown" : weapon },
            };

            SendGameEvent("entity-killed", eventData);
        }

        public static void SendPlayerDeath(
            TakaroPlayer deadPlayer,
            TakaroPlayer attacker,
            Vector3 deathPosition
        )
        {
            if (deadPlayer == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", deadPlayer },
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

            if (attacker != null)
            {
                eventData["attacker"] = attacker;
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
