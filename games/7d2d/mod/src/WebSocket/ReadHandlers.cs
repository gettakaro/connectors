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
                    new Dictionary<string, object>
                    {
                        { "connectable", StateMirror.Instance.IsGameReady },
                    },
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
            PlayerRecord record = StateMirror.Instance.GetPlayerLocationRecord(gameId);
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
                Send(WebSocketMessage.CreateResponse(requestId, new TakaroItem[0]));
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

        public static void ListEntities(string requestId)
        {
            List<TakaroEntity> entities = StateMirror.Instance.GetEntities();
            Send(WebSocketMessage.CreateResponse(requestId, entities.ToArray()));
        }

        public static void ListLocations(string requestId)
        {
            List<TakaroLocation> locations = StateMirror.Instance.GetLocations();
            Send(WebSocketMessage.CreateResponse(requestId, locations.ToArray()));
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

        /// <summary>
        /// Answers Takaro's MapInfoDTO. Previously unimplemented: the router's
        /// default branch replied with a protocol error, which Takaro then tried
        /// to validate as a MapInfoDTO and rejected ("property enabled has failed
        /// the following constraints: isBoolean").
        /// </summary>
        public static void GetMapInfo(string requestId)
        {
            StateMirror mirror = StateMirror.Instance;
            Send(
                WebSocketMessage.CreateResponse(
                    requestId,
                    MapCatalog.BuildMapInfo(
                        mirror.MapRoot,
                        mirror.MapSizeX,
                        mirror.MapSizeY,
                        mirror.MapSizeZ
                    )
                )
            );
        }

        /// <summary>
        /// Serves a single rendered map tile as base64 PNG. Tiles only exist when
        /// the server's web dashboard is enabled; otherwise this reports a clear
        /// error rather than an empty success.
        /// </summary>
        public static void GetMapTile(string requestId, int x, int y, int z)
        {
            string mapRoot = StateMirror.Instance.MapRoot;
            int maxZoom;
            if (!MapCatalog.TryInspect(mapRoot, out maxZoom))
            {
                SendError(
                    requestId,
                    "Map tiles are not available: this server has no web-map tile cache "
                        + "(set WebDashboardEnabled=true in serverconfig.xml and restart)"
                );
                return;
            }

            string tile = MapCatalog.TryReadTileBase64(mapRoot, z, x, y);
            if (tile == null)
            {
                SendError(requestId, $"Map tile {z}/{x}/{y} has not been rendered yet");
                return;
            }

            Send(WebSocketMessage.CreateResponse(requestId, tile));
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
