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
                SendError(requestId, "Player not found", ProtocolErrorCodes.NotFound);
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
                SendError(requestId, "Player not found", ProtocolErrorCodes.NotFound);
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
        /// the server renders its map; otherwise this reports a clear
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
                        + "(set EnableMapRendering=true in serverconfig.xml and restart)"
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

        /// <summary>
        /// Serves the icon game.profile declares, from the bytes shipped in the mod.
        /// Takaro retries game_error with backoff and only ever asks for a declared
        /// revision, so the answer always names the revision of these bytes.
        /// </summary>
        public static void GetGameIcon(string requestId)
        {
            Dictionary<string, object> icon = GameProfile.BuildIconResponse();
            if (icon == null)
            {
                SendError(requestId, "The game icon is not available");
                return;
            }

            Send(WebSocketMessage.CreateResponse(requestId, icon));
        }

        /// <summary>
        /// The v1 tile frame of this server, or null when it has no map: the
        /// server neither renders tiles nor has a tile cache. Read at identify
        /// time, so a map switched on later is declared on the next connection.
        /// </summary>
        public static MapFrame CurrentMapFrame()
        {
            StateMirror mirror = StateMirror.Instance;
            int ignoredZoom;
            if (
                !mirror.MapRenderingEnabled
                && !MapCatalog.TryInspect(mirror.MapRoot, out ignoredZoom)
            )
                return null;
            if (string.IsNullOrEmpty(mirror.MapRoot))
                return null;

            int tilePixels;
            int gameMaxZoom;
            MapCatalog.ReadTileLayout(mirror.MapRoot, out tilePixels, out gameMaxZoom);
            // Takaro refuses a tile size outside 64..1024 and a zoom above 20.
            if (tilePixels < 64 || tilePixels > 1024 || gameMaxZoom > 18)
                return null;

            return MapFrame.ForWorld(tilePixels, gameMaxZoom, mirror.MapHalfExtent);
        }

        // The frame the last identify declared; tile requests are answered in it.
        private static volatile MapFrame _declaredMapFrame;

        /// <summary>
        /// The <c>map</c> Capability to declare, or null when there is no map.
        /// Called once per identify; it also fixes the frame tile requests use.
        /// </summary>
        public static Dictionary<string, object> CurrentMapCapability()
        {
            MapFrame frame = CurrentMapFrame();
            _declaredMapFrame = frame;
            return frame?.BuildCapability(StateMirror.Instance.WorldName);
        }

        /// <summary>
        /// Protocol v1 <c>getMapTile {dimension, zoom, x, y}</c>: the game tile
        /// under the Takaro tile, as <c>{mimeType, data}</c>. A tile the game has
        /// not rendered yet is <c>not_found</c>, which Takaro shows as transparent.
        /// </summary>
        public static void GetMapTileV1(string requestId, string dimension, int zoom, int x, int y)
        {
            MapFrame frame = _declaredMapFrame;
            if (frame == null)
            {
                SendError(
                    requestId,
                    "This server has no map: set EnableMapRendering=true in serverconfig.xml",
                    ProtocolErrorCodes.Unsupported
                );
                return;
            }

            if (dimension != MapFrame.DimensionId)
            {
                SendError(
                    requestId,
                    $"7 Days to Die has no map dimension '{dimension}'",
                    ProtocolErrorCodes.NotFound
                );
                return;
            }

            int gameZoom;
            int gameX;
            int gameY;
            if (!frame.TryToGameTile(zoom, x, y, out gameZoom, out gameX, out gameY))
            {
                SendError(
                    requestId,
                    $"Map tile {zoom}/{x}/{y} is outside the declared pyramid",
                    ProtocolErrorCodes.InvalidArgs
                );
                return;
            }

            string tile = MapCatalog.TryReadTileBase64(
                StateMirror.Instance.MapRoot,
                gameZoom,
                gameX,
                gameY
            );
            if (tile == null)
            {
                SendError(
                    requestId,
                    $"Map tile {zoom}/{x}/{y} (game tile {gameZoom}/{gameX}/{gameY}) has not been rendered yet",
                    ProtocolErrorCodes.NotFound
                );
                return;
            }

            Send(
                WebSocketMessage.CreateResponse(
                    requestId,
                    new Dictionary<string, object> { { "mimeType", "image/png" }, { "data", tile } }
                )
            );
        }

        private static void Send(WebSocketMessage message)
        {
            WebSocketTransport.Instance.Send(message);
        }

        private static void SendError(
            string requestId,
            string errorMessage,
            string code = ProtocolErrorCodes.GameError
        )
        {
            WebSocketTransport.Instance.SendErrorResponse(requestId, errorMessage, code);
        }
    }
}
