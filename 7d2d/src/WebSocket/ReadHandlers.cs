using System.Collections.Generic;
using Takaro.Persistence;
using Takaro.Services;
using UnityEngine;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Read requests are answered entirely from the state mirror (LiteDB) on the
    /// WebSocket thread — they never touch game APIs and never block the game.
    /// </summary>
    public static class ReadHandlers
    {
        public static void TestReachability(string requestId)
        {
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object> { { "connectable", true } },
                    requestId
                )
            );
        }

        public static void GetPlayers(string requestId)
        {
            List<TakaroPlayer> players = StateMirror.Instance.GetOnlinePlayers();
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    players.ToArray(),
                    requestId
                )
            );
        }

        public static void GetPlayer(string requestId, string gameId)
        {
            PlayerRecord record = StateMirror.Instance.GetOnlinePlayer(gameId);
            if (record == null)
            {
                SendError(requestId, "Player not found");
                return;
            }

            TakaroPlayer takaroPlayer = Shared.TransformPlayerRecordToTakaroPlayer(record);
            Send(WebSocketMessage.CreateResponse(requestId, takaroPlayer));
        }

        public static void GetPlayerLocation(string requestId, string gameId)
        {
            PlayerRecord record = StateMirror.Instance.GetOnlinePlayer(gameId);
            if (record == null)
            {
                SendError(requestId, "Player not found");
                return;
            }

            // Vector3i reproduces the exact integer rounding of the previous
            // live-entity read; it is a pure struct, safe off the game thread.
            Vector3i pos = new Vector3i(new Vector3(record.X, record.Y, record.Z));
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    new Dictionary<string, object>
                    {
                        { "x", pos.x },
                        { "y", pos.y },
                        { "z", pos.z },
                    },
                    requestId
                )
            );
        }

        public static void GetPlayerInventory(string requestId, string gameId)
        {
            PlayerRecord record = StateMirror.Instance.GetOnlinePlayer(gameId);
            if (record == null)
            {
                SendError(requestId, "Player not found");
                return;
            }

            List<TakaroItem> items =
                StateMirror.Instance.GetPlayerInventory(gameId) ?? new List<TakaroItem>();
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    items.ToArray(),
                    requestId
                )
            );
        }

        public static void ListItems(string requestId)
        {
            List<TakaroItem> items = StateMirror.Instance.GetItems();
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    items.ToArray(),
                    requestId
                )
            );
        }

        public static void ListBans(string requestId)
        {
            List<TakaroBan> bans = StateMirror.Instance.GetBans();
            Send(
                WebSocketMessage.Create(
                    WebSocketMessage.MessageTypes.Response,
                    bans.ToArray(),
                    requestId
                )
            );
        }

        private static void Send(WebSocketMessage message)
        {
            WebSocketTransport.Instance.Send(message);
        }

        private static void SendError(string requestId, string errorMessage)
        {
            WebSocketTransport.Instance.SendErrorResponse(requestId, errorMessage);
        }
    }
}
