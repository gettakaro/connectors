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
                GameEventTypes.PlayerConnected,
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
                GameEventTypes.PlayerDisconnected,
                new Dictionary<string, object> { { "player", player } }
            );
        }

        // Takaro accepts only global/team/friends/whisper; anything else is global.
        public static string ChatChannelFor(EChatType type)
        {
            switch (type)
            {
                case EChatType.Whisper:
                    return "whisper";
                case EChatType.Friends:
                    return "friends";
                case EChatType.Party:
                    return "team";
                default:
                    return "global";
            }
        }

        public static void SendChatMessage(
            ClientInfo cInfo,
            EChatType type,
            int _senderId,
            string msg,
            List<int> recipientEntityIds
        )
        {
            // Defence in depth against server-message echo: a chat-message event
            // must always identify a real player. Server/Takaro-originated chat
            // uses entity id -1 and the '-non-player-' sender, and must never be
            // republished to Takaro.
            if (cInfo == null || _senderId < 0)
                return;

            SendGameEvent(
                GameEventTypes.ChatMessage,
                new Dictionary<string, object>
                {
                    { "player", Shared.TransformClientInfoToTakaroPlayer(cInfo) },
                    { "msg", msg },
                    { "channel", ChatChannelFor(type) },
                }
            );
        }

        public static void SendEntityKilled(
            TakaroPlayer killer,
            string entityName,
            string weapon = null
        )
        {
            if (killer == null)
                return;

            var eventData = new Dictionary<string, object>
            {
                { "player", killer },
                { "entity", string.IsNullOrEmpty(entityName) ? "unknown" : entityName },
                { "weapon", string.IsNullOrEmpty(weapon) ? "unknown" : weapon },
            };

            SendGameEvent(GameEventTypes.EntityKilled, eventData);
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

            SendGameEvent(GameEventTypes.PlayerDeath, eventData);
        }

        public static void SendLogEvent(string logMessage)
        {
            if (string.IsNullOrEmpty(logMessage))
                return;

            SendGameEvent(
                GameEventTypes.Log,
                new Dictionary<string, object> { { "msg", logMessage } }
            );
        }
    }
}
