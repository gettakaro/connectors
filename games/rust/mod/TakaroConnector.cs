using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Oxide.Core;
using UnityEngine;

namespace Oxide.Plugins
{
    [Info("TakaroConnector", "Takaro", "0.4.0")] // x-release-please-version
    [Description("Takaro Generic Connector — connects outbound to Takaro via WebSocket")]
    public class TakaroConnector : RustPlugin
    {
        // --- Configuration ---

        // takaro:config-begin
        // The plugin's config is <framework config dir>/TakaroConnector.json, created on first
        // load. Environment variables, when set and non-empty, win over the file field by
        // field and are never written to it.
        internal const string DefaultWsUrl = "wss://connect.takaro.io/";
        internal const string EnvWsUrl = "TAKARO_WS_URL";
        internal const string EnvRegistrationToken = "TAKARO_REGISTRATION_TOKEN";
        internal const string EnvIdentityToken = "TAKARO_IDENTITY_TOKEN";
        internal const string EnvDebug = "TAKARO_DEBUG";
        internal const string EnvServerChatName = "TAKARO_SERVER_CHAT_NAME";

        internal sealed class ConnectorSettings
        {
            public string WsUrl = DefaultWsUrl;
            public string RegistrationToken = "";
            public string IdentityToken = "";
            public bool Debug;
            public string ServerChatName = "";
            public bool RegistrationFromEnv;
            public bool IdentityFromEnv;

            public bool SameConnection(ConnectorSettings other)
            {
                return other != null
                    && WsUrl == other.WsUrl
                    && RegistrationToken == other.RegistrationToken
                    && IdentityToken == other.IdentityToken;
            }
        }

        // What the file says; null means the key is absent.
        internal sealed class ConfigFileValues
        {
            public string RegistrationToken;
            public string IdentityToken;
            public string WsUrl;
            public bool? Debug;
            public string ServerChatName;

            public bool HasAnyKey
            {
                get { return RegistrationToken != null || IdentityToken != null || WsUrl != null || Debug != null || ServerChatName != null; }
            }
        }

        // Throws on anything that is not a JSON object: the caller keeps its settings.
        internal static ConfigFileValues ParseConfigText(string text)
        {
            var json = JObject.Parse(text);
            var values = new ConfigFileValues
            {
                RegistrationToken = StringField(json, "RegistrationToken"),
                IdentityToken = StringField(json, "IdentityToken"),
                WsUrl = StringField(json, "WebSocketUrl"),
                ServerChatName = StringField(json, "ServerChatName"),
            };
            var debug = json["Debug"];
            if (debug != null && debug.Type == JTokenType.Boolean)
                values.Debug = debug.Value<bool>();
            else if (debug != null && debug.Type == JTokenType.String)
                values.Debug = string.Equals(debug.Value<string>()?.Trim(), "true", StringComparison.OrdinalIgnoreCase);
            return values;
        }

        private static string StringField(JObject json, string key)
        {
            var token = json[key];
            if (token == null) return null;
            if (token.Type == JTokenType.Null) return "";
            return (token.Type == JTokenType.String ? token.Value<string>() : token.ToString()).Trim();
        }

        internal static string RenderConfigText(ConfigFileValues values)
        {
            var json = new JObject
            {
                ["RegistrationToken"] = values.RegistrationToken ?? "",
                ["IdentityToken"] = values.IdentityToken ?? "",
                ["WebSocketUrl"] = string.IsNullOrEmpty(values.WsUrl) ? DefaultWsUrl : values.WsUrl,
                ["Debug"] = values.Debug ?? false,
                ["ServerChatName"] = values.ServerChatName ?? "",
            };
            return json.ToString(Formatting.Indented) + "\n";
        }

        // While running: a missing, empty or unparseable file is one being saved or edited,
        // so it yields nothing and the current settings stay.
        internal static bool TryParseRunningConfig(string text, out ConfigFileValues values)
        {
            values = null;
            if (string.IsNullOrWhiteSpace(text)) return false;
            try { values = ParseConfigText(text); }
            catch { return false; }
            return true;
        }

        // A file the plugin never wrote: absent, empty, or the "{}" a framework may leave.
        internal static bool IsFreshConfig(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return true;
            try { return !ParseConfigText(text).HasAnyKey; }
            catch { return false; }
        }

        // The file a first load writes: no token yet and a new identity.
        internal static ConfigFileValues FirstConfig(Func<string> newIdentity)
        {
            return new ConfigFileValues
            {
                RegistrationToken = "",
                IdentityToken = newIdentity(),
                WsUrl = DefaultWsUrl,
                Debug = false,
                ServerChatName = "",
            };
        }

        // The file text with IdentityToken set; every other key stays as it was.
        internal static string WithIdentity(string text, string identity)
        {
            var json = JObject.Parse(text);
            json["IdentityToken"] = identity;
            return json.ToString(Formatting.Indented) + "\n";
        }

        // `current` is what is in use (null on first load): an absent key while running is a
        // file being edited, so it keeps the current token instead of dropping it.
        internal static ConnectorSettings ResolveSettings(ConfigFileValues file, Func<string, string> env, ConnectorSettings current)
        {
            var settings = new ConnectorSettings();

            var envUrl = env(EnvWsUrl);
            settings.WsUrl = !string.IsNullOrEmpty(envUrl) ? envUrl
                : !string.IsNullOrEmpty(file.WsUrl) ? file.WsUrl
                : DefaultWsUrl;

            var envRegistration = env(EnvRegistrationToken);
            settings.RegistrationFromEnv = !string.IsNullOrEmpty(envRegistration);
            settings.RegistrationToken = settings.RegistrationFromEnv ? envRegistration
                : file.RegistrationToken ?? (current != null && !current.RegistrationFromEnv ? current.RegistrationToken : "");

            var envIdentity = env(EnvIdentityToken);
            settings.IdentityFromEnv = !string.IsNullOrEmpty(envIdentity);
            settings.IdentityToken = settings.IdentityFromEnv ? envIdentity
                : file.IdentityToken ?? (current != null && !current.IdentityFromEnv ? current.IdentityToken : "");

            var envDebug = env(EnvDebug);
            settings.Debug = !string.IsNullOrEmpty(envDebug)
                ? envDebug.ToLower() == "true"
                : file.Debug ?? false;

            // Not a connection setting: a change applies to the next message, no reconnect.
            var envChatName = env(EnvServerChatName)?.Trim();
            settings.ServerChatName = !string.IsNullOrEmpty(envChatName) ? envChatName : file.ServerChatName ?? "";
            return settings;
        }
        // Takaro's identify errors can carry its own internal request, x-takaro-token JWT and
        // all: only name, message and HTTP status are logged, and anything shaped like a JWT
        // is cut out of them.
        internal static string IdentifyErrorText(JToken error)
        {
            if (error == null || error.Type == JTokenType.Null) return "unknown error";
            if (error.Type != JTokenType.Object) return RedactJwt(error.ToString());

            var obj = (JObject)error;
            var parts = new List<string>();
            foreach (var key in new[] { "name", "message" })
            {
                var value = obj[key];
                if (value != null && value.Type == JTokenType.String && !string.IsNullOrEmpty(value.Value<string>()))
                    parts.Add(value.Value<string>());
            }
            foreach (var key in new[] { "status", "statusCode", "httpCode" })
            {
                var value = obj[key];
                if (value != null && (value.Type == JTokenType.Integer || value.Type == JTokenType.String))
                {
                    parts.Add("HTTP " + value);
                    break;
                }
            }
            return parts.Count == 0 ? "unknown error" : RedactJwt(string.Join(": ", parts));
        }

        internal static string RedactJwt(string text)
        {
            return System.Text.RegularExpressions.Regex.Replace(text ?? "",
                @"eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+", "<redacted token>");
        }

        // The sender players see on chat from Takaro: senderNameOverride (newer Takaro fills it
        // from the server's serverChatName setting), else the ServerChatName setting, else
        // null for no sender at all. Blank values count as unset.
        internal static string ResolveServerChatName(string senderNameOverride, string configuredName)
        {
            var name = senderNameOverride?.Trim();
            if (!string.IsNullOrEmpty(name)) return name;
            name = configuredName?.Trim();
            return string.IsNullOrEmpty(name) ? null : name;
        }

        internal const string ServerChatNameColor = "#eee";

        // The client command that shows a server chat line, for a broadcast and a whisper alike.
        // With a sender it is chat.add2 on the Server channel, which the client draws as
        // "Name: text" with the name in its own colour, the grey Rust uses for SERVER. Without
        // one it is chat.add, exactly what the plugin always sent: the text alone.
        internal static object[] ServerChatCommand(string senderName, string message, out string command)
        {
            if (senderName == null)
            {
                command = "chat.add";
                return new object[] { 2, 0, message };
            }
            command = "chat.add2";
            return new object[] { 2, 0, message, EscapeChatName(senderName), ServerChatNameColor, 1f };
        }

        // Rust's EscapeRichText: a zero-width space keeps < and > in a name from being read
        // as rich-text tags, so a name can neither restyle nor hide the message.
        internal static string EscapeChatName(string name)
        {
            return name.Replace("<", "<\u200b").Replace(">", "\u200b>").Replace("\\", "\\\u200b");
        }
        // takaro:config-end

        // takaro:map-begin
        //
        // World map for Takaro's getMapInfo/getMapTile (protocol v0). The tiles follow the
        // 7 Days to Die web-map layout the Takaro player map already draws: 128 px tiles,
        // signed tile indices centred on the world origin, tile x grows east (+X), tile y
        // grows north (+Z), and at MapMaxZoom one pixel is one metre. At zoom z one tile
        // therefore spans 128 * 2^(MapMaxZoom - z) metres. Everything in this region is free
        // of Unity and game types so tests/map can compile and run it on its own.

        internal const int MapTilePixels = 128;
        internal const int MapMaxZoom = 4;

        // Ocean served around the world square, in metres. The in-game map shows open sea
        // past the edge too, and players can swim or sail out there.
        internal const float MapOceanPad = 512f;

        // At most this many raster pixels along one side; bigger worlds render at less than
        // one pixel per metre. Rendering is the game's own, on the main thread, and its cost
        // grows with the pixel count.
        internal const int MapMaxRasterSide = 2048;

        /// <summary>
        /// The world map as one RGB raster. Row 0 is the southern edge (Unity texture order),
        /// column 0 the western edge. Immutable once built.
        /// </summary>
        internal sealed class MapRaster
        {
            public readonly byte[] Rgb;
            public readonly int Width;
            public readonly int Height;
            public readonly float WorldSize;
            public readonly float MinX;
            public readonly float MinZ;
            public readonly float PixelsPerMetre;
            public readonly byte BackgroundR;
            public readonly byte BackgroundG;
            public readonly byte BackgroundB;
            public readonly string Key;

            public MapRaster(byte[] rgb, int width, int height, float worldSize, float minX, float minZ,
                float pixelsPerMetre, byte backgroundR, byte backgroundG, byte backgroundB, string key)
            {
                if (width <= 0 || height <= 0 || rgb == null || rgb.Length != width * height * 3)
                    throw new ArgumentException("map raster size does not match its pixels");
                if (!(pixelsPerMetre > 0f) || !(worldSize > 0f))
                    throw new ArgumentException("map raster scale must be positive");
                Rgb = rgb;
                Width = width;
                Height = height;
                WorldSize = worldSize;
                MinX = minX;
                MinZ = minZ;
                PixelsPerMetre = pixelsPerMetre;
                BackgroundR = backgroundR;
                BackgroundG = backgroundG;
                BackgroundB = backgroundB;
                Key = key ?? "";
            }
        }

        /// <summary>Raster pixels per metre for a world of this size: 1, or less for big worlds.</summary>
        internal static float MapRenderScale(float worldSize)
        {
            if (!(worldSize > 0f)) return 1f;
            return Math.Min(1f, Math.Max(0.25f, MapMaxRasterSide / worldSize));
        }

        internal static double MapTileWorldSize(int zoom)
        {
            return MapTilePixels * Math.Pow(2, MapMaxZoom - zoom);
        }

        /// <summary>True when the tile overlaps the world square plus its ocean pad.</summary>
        internal static bool MapTileInRange(float worldSize, int zoom, int x, int y)
        {
            if (zoom < 0 || zoom > MapMaxZoom || !(worldSize > 0f)) return false;
            var size = MapTileWorldSize(zoom);
            var edge = worldSize / 2.0 + MapOceanPad;
            var minX = x * size;
            var minZ = y * size;
            return minX < edge && minX + size > -edge && minZ < edge && minZ + size > -edge;
        }

        /// <summary>
        /// Payload for Takaro's MapInfoDTO. mapBlockSize is the world size of one tile at
        /// maxZoom. Every mapSize* is the horizontal extent so readers that take X/Y and those
        /// that take X/Z as the ground plane both get it, and the vertical axis reads as Y.
        /// </summary>
        internal static JObject MapInfo(bool enabled, float worldSize)
        {
            var extent = (int)Math.Round(worldSize > 0f ? worldSize : 0f);
            return new JObject
            {
                ["enabled"] = enabled,
                ["mapBlockSize"] = MapTilePixels,
                ["maxZoom"] = MapMaxZoom,
                ["mapSizeX"] = extent,
                ["mapSizeY"] = extent,
                ["mapSizeZ"] = extent
            };
        }

        /// <summary>
        /// Reads getMapTile's {x, y, z} (z may come as "zoom"; numbers may come as strings).
        /// Returns null when they are usable, or the reason they are not.
        /// </summary>
        internal static string ParseMapTileArgs(JObject args, out int x, out int y, out int zoom)
        {
            x = 0;
            y = 0;
            zoom = 0;
            if (args == null) return "x, y and z are required";
            if (!MapInt(args["x"], out x)) return "x must be an integer";
            if (!MapInt(args["y"], out y)) return "y must be an integer";
            var z = args["z"];
            if (z == null || z.Type == JTokenType.Null) z = args["zoom"];
            if (!MapInt(z, out zoom)) return "z must be an integer";
            if (zoom < 0 || zoom > MapMaxZoom) return $"z must be between 0 and {MapMaxZoom}";
            return null;
        }

        private static bool MapInt(JToken token, out int value)
        {
            value = 0;
            if (token == null) return false;
            if (token.Type == JTokenType.Integer)
            {
                var n = token.Value<long>();
                if (n < int.MinValue || n > int.MaxValue) return false;
                value = (int)n;
                return true;
            }
            if (token.Type == JTokenType.Float)
            {
                var d = token.Value<double>();
                if (d != Math.Floor(d) || d < int.MinValue || d > int.MaxValue) return false;
                value = (int)d;
                return true;
            }
            if (token.Type == JTokenType.String)
                return int.TryParse(token.Value<string>(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
            return false;
        }

        /// <summary>
        /// One tile as RGB, row 0 = the tile's southern edge (Unity texture order, which
        /// EncodeToPNG writes as the bottom row). Each output pixel averages up to 4x4 raster
        /// samples so zoomed-out tiles do not alias.
        /// </summary>
        internal static byte[] RenderMapTileRgb(MapRaster raster, int zoom, int x, int y)
        {
            var size = MapTileWorldSize(zoom);
            return RenderMapAreaRgb(raster, x * size, y * size, size);
        }

        /// <summary>The square world area whose south-west corner is (originX, originZ).</summary>
        internal static byte[] RenderMapAreaRgb(MapRaster raster, double originX, double originZ, double size)
        {
            const int n = MapTilePixels;
            var rgb = new byte[n * n * 3];
            var metresPerPixel = size / n;
            var samples = (int)Math.Max(1, Math.Min(4, Math.Ceiling(metresPerPixel * raster.PixelsPerMetre)));
            var step = metresPerPixel / samples;
            for (var row = 0; row < n; row++)
            {
                for (var column = 0; column < n; column++)
                {
                    int r = 0, g = 0, b = 0;
                    for (var sz = 0; sz < samples; sz++)
                    {
                        var wz = originZ + row * metresPerPixel + (sz + 0.5) * step;
                        for (var sx = 0; sx < samples; sx++)
                        {
                            var wx = originX + column * metresPerPixel + (sx + 0.5) * step;
                            var index = MapRasterIndex(raster, wx, wz);
                            if (index < 0)
                            {
                                r += raster.BackgroundR;
                                g += raster.BackgroundG;
                                b += raster.BackgroundB;
                            }
                            else
                            {
                                r += raster.Rgb[index];
                                g += raster.Rgb[index + 1];
                                b += raster.Rgb[index + 2];
                            }
                        }
                    }
                    var count = samples * samples;
                    var offset = (row * n + column) * 3;
                    rgb[offset] = (byte)((r + count / 2) / count);
                    rgb[offset + 1] = (byte)((g + count / 2) / count);
                    rgb[offset + 2] = (byte)((b + count / 2) / count);
                }
            }
            return rgb;
        }

        /// <summary>Byte offset of the raster pixel under a world point, or -1 outside the raster.</summary>
        internal static int MapRasterIndex(MapRaster raster, double worldX, double worldZ)
        {
            var px = (int)Math.Floor((worldX - raster.MinX) * raster.PixelsPerMetre);
            var py = (int)Math.Floor((worldZ - raster.MinZ) * raster.PixelsPerMetre);
            if (px < 0 || py < 0 || px >= raster.Width || py >= raster.Height) return -1;
            return (py * raster.Width + px) * 3;
        }

        /// <summary>
        /// Names the world a raster was drawn from, so a wipe, a new seed or size, a game
        /// update that changes procedural generation, or a custom map never reuses an old
        /// render. Safe as a file name.
        /// </summary>
        internal static string MapWorldKey(uint size, uint seed, string mapFileName, string checksum)
        {
            var source = (mapFileName ?? "") + "|" + (checksum ?? "");
            ulong hash = 14695981039346656037UL;
            foreach (var c in source)
            {
                hash ^= c;
                hash *= 1099511628211UL;
            }
            return $"{size}_{seed}_{hash:x16}";
        }

        // Protocol v1 asks for tiles of a declared Dimension: {dimension, zoom, x, y} on the
        // XYZ web-map layout, zoom 0 being one tile over the whole bounds square, tile y
        // growing down (south). Rust's world is x east, z north, so the one Dimension is
        // drawn with x to the right and z up, over the world square plus its ocean pad.
        internal const string MapDimensionId = "world";

        /// <summary>A getPlayerLocation payload. On protocol v1 it names the one Dimension, so
        /// Takaro files the position under the same id the map and the locations declare.</summary>
        internal static JObject PlayerLocationPayload(float x, float y, float z, int protocol)
        {
            var payload = new JObject { ["x"] = x, ["y"] = y, ["z"] = z };
            if (protocol >= 1) payload["dimension"] = MapDimensionId;
            return payload;
        }

        /// <summary>The getPlayerLocation answer when the player is not on the server and no
        /// position is cached. v1 answers null so Takaro keeps the last real position; v0 keeps
        /// its historic (0, 0, 0).</summary>
        internal static JObject UnknownPlayerLocation(int protocol)
        {
            return protocol >= 1 ? null : PlayerLocationPayload(0, 0, 0, protocol);
        }

        internal static float MapV1Min(MapRaster raster) { return Math.Min(raster.MinX, raster.MinZ) - MapOceanPad; }
        internal static float MapV1Side(MapRaster raster) { return raster.WorldSize + 2 * MapOceanPad; }

        /// <summary>The smallest zoom at which one tile pixel is at most one metre.</summary>
        internal static int MapV1MaxZoom(float side)
        {
            var zoom = 0;
            while (zoom < 20 && side / Math.Pow(2, zoom) / MapTilePixels > 1.0) zoom++;
            return zoom;
        }

        internal static JObject MapCapability(MapRaster raster)
        {
            var min = MapV1Min(raster);
            var max = min + MapV1Side(raster);
            var bounds = new JObject { ["min"] = min, ["max"] = max };
            return new JObject
            {
                ["name"] = "map",
                ["parameters"] = new JObject
                {
                    ["dimensions"] = new JArray
                    {
                        new JObject
                        {
                            ["id"] = MapDimensionId,
                            ["name"] = "World",
                            ["axes"] = new JObject
                            {
                                ["horizontal"] = new JObject { ["field"] = "x", ["direction"] = "right" },
                                ["vertical"] = new JObject { ["field"] = "z", ["direction"] = "up" },
                            },
                            ["bounds"] = new JObject { ["horizontal"] = bounds, ["vertical"] = bounds.DeepClone() },
                            ["source"] = new JObject
                            {
                                ["type"] = "tiled",
                                ["tileSize"] = MapTilePixels,
                                ["minZoom"] = 0,
                                ["maxZoom"] = MapV1MaxZoom(MapV1Side(raster)),
                            },
                        },
                    },
                },
            };
        }

        /// <summary>
        /// Reads a v1 getMapTile's {dimension, zoom, x, y}. Returns null when they name a tile
        /// of this map, or the reason they do not.
        /// </summary>
        internal static string ParseMapTileV1Args(JObject args, float side, out int zoom, out int x, out int y)
        {
            zoom = 0;
            x = 0;
            y = 0;
            if (args == null) return "dimension, zoom, x and y are required";
            var dimension = args.Value<string>("dimension");
            if (dimension != MapDimensionId) return $"unknown dimension '{dimension}'";
            if (!MapInt(args["zoom"], out zoom)) return "zoom must be an integer";
            if (!MapInt(args["x"], out x)) return "x must be an integer";
            if (!MapInt(args["y"], out y)) return "y must be an integer";
            var maxZoom = MapV1MaxZoom(side);
            if (zoom < 0 || zoom > maxZoom) return $"zoom must be between 0 and {maxZoom}";
            var tiles = 1 << zoom;
            if (x < 0 || y < 0 || x >= tiles || y >= tiles) return $"tile {zoom}/{x}/{y} is outside the map";
            return null;
        }

        /// <summary>A v1 tile as RGB, row 0 = the tile's southern edge like RenderMapTileRgb.</summary>
        internal static byte[] RenderMapTileV1Rgb(MapRaster raster, int zoom, int x, int y)
        {
            var size = MapV1Side(raster) / Math.Pow(2, zoom);
            var west = MapV1Min(raster) + x * size;
            var north = MapV1Min(raster) + MapV1Side(raster) - y * size;
            return RenderMapAreaRgb(raster, west, north - size, size);
        }
        // takaro:map-end

        // takaro:protocol-begin
        //
        // Generic Game Protocol v1 next to v0: what identify offers, how Takaro's answer is
        // read, the error shapes of both versions and the numbered event stream. Free of Unity
        // and game types so tests/protocol can compile and run it on its own.

        internal static readonly int[] OfferedProtocolVersions = { 0, 1 };

        // testReachability is always allowed. getMapInfo has no v1 capability, and getMapTile
        // belongs to the map capability, which is declared once the map raster is ready.
        internal static readonly string[][] ActionCapabilities =
        {
            new[] { "getPlayers", "players.list" },
            new[] { "getPlayer", "players.get" },
            new[] { "getPlayerLocation", "players.location" },
            new[] { "getPlayerInventory", "players.inventory" },
            new[] { "teleportPlayer", "players.teleport" },
            new[] { "listItems", "items.list" },
            new[] { "giveItem", "items.give" },
            new[] { "listEntities", "entities.list" },
            new[] { "listLocations", "locations.list" },
            new[] { "sendMessage", "chat.send" },
            new[] { "executeConsoleCommand", "console.execute" },
            new[] { "kickPlayer", "moderation.kick" },
            new[] { "banPlayer", "moderation.ban" },
            new[] { "unbanPlayer", "moderation.unban" },
            new[] { "listBans", "moderation.listBans" },
            new[] { "shutdown", "server.shutdown" },
        };

        internal static readonly string[] GameEventTypes =
        {
            "player-connected", "player-disconnected", "chat-message", "player-death", "entity-killed", "log",
        };

        internal const string ErrorUnsupported = "unsupported";
        internal const string ErrorInvalidArgs = "invalid_args";
        internal const string ErrorNotFound = "not_found";
        internal const string ErrorGameError = "game_error";
        internal const string ErrorInternal = "internal";

        /// <summary>
        /// A request failure with its v1 code. V0Message is what a v0 connection gets, where
        /// errors are free text; it defaults to Message.
        /// </summary>
        internal sealed class ProtocolError : Exception
        {
            public readonly string Code;
            public readonly string V0Message;

            public ProtocolError(string code, string message, string v0Message = null) : base(message)
            {
                Code = code;
                V0Message = v0Message ?? message;
            }
        }

        /// <summary>The full manifest. `extra` are the optional capabilities (map); nulls are skipped.</summary>
        internal static JArray BuildManifest(params JObject[] extra)
        {
            var manifest = new JArray();
            foreach (var pair in ActionCapabilities)
                manifest.Add(new JObject { ["name"] = pair[1] });
            foreach (var eventType in GameEventTypes)
                manifest.Add(new JObject { ["name"] = "events." + eventType });
            foreach (var capability in extra)
                if (capability != null) manifest.Add(capability);
            return manifest;
        }

        internal static bool IsDeclaredAction(string action)
        {
            foreach (var pair in ActionCapabilities)
                if (pair[0] == action) return true;
            return false;
        }

        // The game and connector as Takaro records them. The slugs are stable: Takaro groups
        // connector releases by connector name.
        internal const string GameIdentifier = "rust";
        internal const string ConnectorName = "takaro-rust";
        internal const string GameDisplayName = "Rust";

        /// <summary>
        /// The identify payload. `name` (the server's hostname) is sent when it has at least 3
        /// characters; `connectorVersion` only when it is strict semver, since Takaro refuses
        /// the whole identify over a malformed one.
        /// </summary>
        internal static JObject BuildIdentifyPayload(string identityToken, string registrationToken, JArray manifest,
            string name = null, string connectorVersion = null)
        {
            var payload = new JObject
            {
                ["identityToken"] = identityToken ?? "",
                ["registrationToken"] = registrationToken ?? "",
                ["game"] = GameIdentifier,
                ["protocolVersions"] = new JArray(OfferedProtocolVersions),
                ["capabilities"] = manifest,
            };
            if (!string.IsNullOrEmpty(name) && name.Trim().Length >= 3)
                payload["name"] = name.Trim();
            if (IsStrictSemver(connectorVersion))
                payload["connector"] = new JObject { ["name"] = ConnectorName, ["version"] = connectorVersion };
            return payload;
        }

        internal static bool IsStrictSemver(string version)
        {
            return !string.IsNullOrEmpty(version) && version.Length <= 64 && System.Text.RegularExpressions.Regex.IsMatch(version,
                @"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-((0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(\.(0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(\+[0-9a-zA-Z-]+(\.[0-9a-zA-Z-]+)*)?$");
        }

        /// <summary>The game build as Takaro takes it: trimmed printable ASCII, at most 32 characters, or null.</summary>
        internal static string GameVersionLabel(string raw)
        {
            if (raw == null) return null;
            var text = new string(raw.Where(c => c >= 0x20 && c <= 0x7E).ToArray()).Trim();
            if (text.Length > 32) text = text.Substring(0, 32).Trim();
            return text.Length == 0 ? null : text;
        }

        /// <summary>
        /// The shipped icon: its bytes, revision (first 12 hex of the sha256) and size, all
        /// derived from the bytes so a changed icon always gets a new revision.
        /// </summary>
        internal sealed class GameIcon
        {
            public readonly byte[] Bytes;
            public readonly string Revision;
            public readonly string Base64;

            public GameIcon(byte[] bytes)
            {
                Bytes = bytes;
                Base64 = Convert.ToBase64String(bytes);
                using (var sha = System.Security.Cryptography.SHA256.Create())
                    Revision = string.Concat(sha.ComputeHash(bytes).Take(6).Select(b => b.ToString("x2")));
            }

            public int Size { get { return Bytes.Length; } }

            // Base64 ignores the line breaks the embedded constant is wrapped with.
            public static GameIcon FromBase64(string text) { return new GameIcon(Convert.FromBase64String(text)); }
        }

        internal static JObject GameProfileCapability(string gameVersion, GameIcon icon)
        {
            var parameters = new JObject { ["name"] = GameDisplayName };
            var version = GameVersionLabel(gameVersion);
            if (version != null) parameters["version"] = version;
            if (icon != null) parameters["icon"] = new JObject { ["revision"] = icon.Revision, ["size"] = icon.Size };
            return new JObject { ["name"] = "game.profile", ["parameters"] = parameters };
        }

        // The whole PNG in one answer, under the revision the manifest declared.
        internal static JObject GameIconResponse(GameIcon icon)
        {
            return new JObject { ["revision"] = icon.Revision, ["data"] = icon.Base64 };
        }

        internal sealed class IdentifyReply
        {
            public bool Accepted;
            public string GameServerId;
            public int ProtocolVersion;
            // The v1 rejection code; null for a v0 rejection, which has none.
            public string ErrorCode;
            public string Reason;
            // Takaro failed on its side ("internal"); the token is fine, so retry with backoff.
            public bool Retryable => !Accepted && ErrorCode == ErrorInternal;
        }

        // A v1 rejection is a top-level error (payload null, socket closed 1008); a v0 one
        // is payload.error with the socket left open. A missing protocolVersion is v0.
        internal static IdentifyReply ParseIdentifyResponse(JObject frame)
        {
            var topError = frame["error"];
            if (topError != null && topError.Type != JTokenType.Null && !(topError.Type == JTokenType.Boolean && !topError.Value<bool>()))
            {
                var code = topError.Type == JTokenType.Object ? (topError as JObject).Value<string>("code") : null;
                return new IdentifyReply { ErrorCode = code, Reason = (code != null ? code + ": " : "") + IdentifyErrorText(topError) };
            }

            var payload = frame["payload"] as JObject ?? new JObject();
            var error = payload["error"];
            if (error != null && error.Type != JTokenType.Null)
                return new IdentifyReply { Reason = IdentifyErrorText(error) };

            var version = 0;
            var versionToken = payload["protocolVersion"];
            if (versionToken != null && versionToken.Type != JTokenType.Null)
            {
                if (versionToken.Type != JTokenType.Integer || Array.IndexOf(OfferedProtocolVersions, versionToken.Value<int>()) < 0)
                    return new IdentifyReply
                    {
                        ErrorCode = "no_shared_protocol_version",
                        Reason = $"Takaro selected protocol version {versionToken}, which this plugin does not speak",
                    };
                version = versionToken.Value<int>();
            }

            return new IdentifyReply
            {
                Accepted = true,
                ProtocolVersion = version,
                GameServerId = payload["gameServerId"]?.Value<string>() ?? (payload["server"] as JObject)?.Value<string>("id"),
            };
        }

        /// <summary>
        /// A response frame. Success: the payload, where null means "nothing to return" (v1
        /// sends null, v0 keeps its empty object). Failure: v1 {code, message}, v0 a string.
        /// </summary>
        internal static JObject ResponseFrame(int protocolVersion, string requestId, JToken payload, ProtocolError error)
        {
            var frame = new JObject { ["type"] = "response", ["requestId"] = requestId };
            if (error != null)
            {
                frame["error"] = protocolVersion >= 1
                    ? (JToken)new JObject { ["code"] = error.Code, ["message"] = error.Message }
                    : error.V0Message;
                return frame;
            }
            frame["payload"] = payload ?? (protocolVersion >= 1 ? JValue.CreateNull() : (JToken)new JObject());
            return frame;
        }

        /// <summary>
        /// The game events of this process as one v1 stream: numbered from 1 under a streamId
        /// made at load, buffered until Takaro acks them, the oldest dropped past Capacity.
        /// Thread-safe; Add runs on the game thread and never blocks on the network.
        /// </summary>
        internal sealed class EventStream
        {
            public const int Capacity = 2000;
            public readonly string StreamId;
            private readonly LinkedList<KeyValuePair<long, JObject>> _pending = new LinkedList<KeyValuePair<long, JObject>>();
            private readonly object _lock = new object();
            private long _nextSeq = 1;
            public long Dropped;

            public EventStream(string streamId) { StreamId = streamId; }

            public object Lock { get { return _lock; } }

            /// <summary>Numbers the event and returns its v1 frame; v1 keeps it until acked.</summary>
            public JObject Add(string eventType, JObject data, bool keep)
            {
                lock (_lock)
                {
                    var seq = _nextSeq++;
                    var frame = new JObject
                    {
                        ["type"] = "gameEvent",
                        ["payload"] = new JObject
                        {
                            ["type"] = eventType,
                            ["data"] = data,
                            ["streamId"] = StreamId,
                            ["seq"] = seq,
                        },
                    };
                    if (keep)
                    {
                        _pending.AddLast(new KeyValuePair<long, JObject>(seq, frame));
                        while (_pending.Count > Capacity)
                        {
                            _pending.RemoveFirst();
                            Dropped++;
                        }
                    }
                    return frame;
                }
            }

            public void Ack(string streamId, long upTo)
            {
                if (streamId != StreamId) return;
                lock (_lock)
                {
                    while (_pending.First != null && _pending.First.Value.Key <= upTo)
                        _pending.RemoveFirst();
                }
            }

            /// <summary>The unacknowledged frames in seq order.</summary>
            public List<JObject> Pending()
            {
                lock (_lock)
                    return _pending.Select(p => p.Value).ToList();
            }

            public int PendingCount { get { lock (_lock) return _pending.Count; } }

            /// <summary>A v0 connection has no acks: what is buffered is sent once and forgotten.</summary>
            public List<JObject> TakeAll()
            {
                lock (_lock)
                {
                    var all = _pending.Select(p => p.Value).ToList();
                    _pending.Clear();
                    return all;
                }
            }
        }

        /// <summary>The same event as a v0 frame: no streamId or seq.</summary>
        internal static JObject V0EventFrame(JObject v1Frame)
        {
            var payload = (JObject)v1Frame["payload"];
            return new JObject
            {
                ["type"] = "gameEvent",
                ["payload"] = new JObject { ["type"] = payload["type"], ["data"] = payload["data"] },
            };
        }

        internal static bool TryParseEventAck(JObject frame, out string streamId, out long upTo)
        {
            streamId = null;
            upTo = 0;
            var payload = frame["payload"] as JObject;
            var upToToken = payload?["upTo"];
            streamId = payload?.Value<string>("streamId");
            if (string.IsNullOrEmpty(streamId) || upToToken == null || upToToken.Type != JTokenType.Integer) return false;
            upTo = upToToken.Value<long>();
            return true;
        }
        // takaro:protocol-end

        private const string ConfigFileName = "TakaroConnector.json";
        private const int ConfigCheckEverySeconds = 5;

        private volatile ConnectorSettings _settings = new ConnectorSettings();
        private string _configPath;
        private string _configText;
        private string _configProblem;
        // The identity the file holds; put back when an edit removes it.
        private string _fileIdentity;
        private Timer _tickTimer;
        private int _tickCount;

        private bool _debug { get { return _settings.Debug; } }

        private const long InitialReconnectDelay = 5000;
        private const long MaxReconnectDelay = 300000;
        private const double BackoffMultiplier = 1.5;

        // --- WebSocket State ---
        //
        // Connections are started on the main thread only (Init, the 1 s tick). Every
        // connection carries the generation it was started under; a socket task, close or
        // reconnect request from an older generation is stale and changes nothing.

        private ClientWebSocket _ws;
        private CancellationTokenSource _cts;
        private volatile bool _shouldReconnect = true;
        private long _currentReconnectDelay = InitialReconnectDelay;
        private volatile bool _connected;
        private int _generation;
        private int _identifySentGeneration = -1;
        // An auth close stops retries for that connection only; a config change starts a new one.
        private int _authClosedGeneration = -1;
        // The connection whose identify failed on Takaro's side; its 1008 close is not an auth close.
        private int _retryableRejectGeneration = -1;
        private ReconnectRequest _pendingReconnect;
        // The protocol version Takaro chose for the current connection; -1 until identified.
        private volatile int _protocolVersion = -1;
        // Frames go out one at a time in the order they were queued: v1 events must reach
        // Takaro in seq order, or its high-water mark drops the overtaken ones as duplicates.
        private readonly Queue<OutgoingFrame> _sendQueue = new Queue<OutgoingFrame>();
        private bool _sendDraining;
        // One event stream per process: a reload or restart is a new streamId starting at 1.
        private readonly EventStream _events = new EventStream(Guid.NewGuid().ToString());
        // The map capability, set on the main thread once the raster is ready.
        private volatile JObject _mapCapability;
        // Null only if the embedded icon cannot be decoded; the profile then has no icon.
        private GameIcon _gameIcon;

        private sealed class OutgoingFrame
        {
            public ClientWebSocket Socket;
            public string Text;
            // The connection's token: a reconnect cancels a send stuck on the old socket.
            public CancellationToken Token;
        }

        private sealed class ReconnectRequest
        {
            public int Generation;
            public long DueTicks;
        }
        private readonly Dictionary<string, Vector3> _lastPosition = new Dictionary<string, Vector3>();

        // --- Timed Bans ---
        //
        // Rust's native ban list (ServerUsers) has no expiry, so timed bans are kept
        // in the plugin's own data file and enforced on login. Permanent bans still
        // go through the native ban list.

        private const string BanDataFile = "TakaroConnector_bans";
        private const float BanSweepInterval = 60f;

        private readonly Dictionary<ulong, TimedBan> _timedBans = new Dictionary<ulong, TimedBan>();
        private Timer _banSweepTimer;

        private class TimedBan
        {
            [JsonProperty("steamId")]
            public string SteamId { get; set; }

            [JsonProperty("name")]
            public string Name { get; set; }

            [JsonProperty("reason")]
            public string Reason { get; set; }

            [JsonProperty("expiresAt")]
            public string ExpiresAt { get; set; }
        }

        // --- Lifecycle ---

        private void Init()
        {
            try { _gameIcon = GameIcon.FromBase64(GameIconBase64); }
            catch (Exception ex) { LogWarning($"Game icon: {ex.Message}"); }

            LoadTimedBans();
            _banSweepTimer = timer.Every(BanSweepInterval, () => PruneExpiredBans());

            _configPath = Path.GetFullPath(Path.Combine(Interface.Oxide.ConfigDirectory, ConfigFileName));
            LoadSettings();
            _tickTimer = timer.Every(1f, Tick);

            var settings = _settings;
            LogInfo($"Config: {_configPath} (registration token {TokenSource(settings)})");
            if (settings.RegistrationFromEnv)
                LogInfo($"{EnvRegistrationToken} is set in the server's environment and overrides RegistrationToken in the config file");

            if (string.IsNullOrEmpty(settings.RegistrationToken))
            {
                LogMissingToken();
                return;
            }

            LogInfo($"Connecting to {settings.WsUrl}");
            StartConnection();
        }

        private void Unload()
        {
            _shouldReconnect = false;
            _connected = false;
            _protocolVersion = -1;
            Interlocked.Increment(ref _generation);
            Interlocked.Exchange(ref _pendingReconnect, null);
            _tickTimer?.Destroy();
            _tickTimer = null;
            _cts?.Cancel();
            try { _ws?.Dispose(); } catch { }
            _banSweepTimer?.Destroy();
            _banSweepTimer = null;
            _lastPosition.Clear();
        }

        // --- Config file ---

        private static string EnvValue(string name)
        {
            return Environment.GetEnvironmentVariable(name);
        }

        private static string TokenSource(ConnectorSettings settings)
        {
            if (settings.RegistrationFromEnv) return $"from {EnvRegistrationToken}";
            return string.IsNullOrEmpty(settings.RegistrationToken) ? "not set" : "set";
        }

        private static string ReadConfigText(string path)
        {
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }

        // First load: an absent (or never-written) file is created with an empty token and a
        // new identity; an existing file without an identity gets one written into it.
        private void LoadSettings()
        {
            string text;
            try { text = ReadConfigText(_configPath); }
            catch (Exception ex)
            {
                // Never recreate a file that exists but could not be read: that would replace
                // its token and identity. The 5 s check picks it up once it reads.
                LogWarning($"Could not read {_configPath} ({ex.Message}); trying again in a few seconds");
                _configText = null;
                _configProblem = "";
                _settings = ResolveSettings(new ConfigFileValues(), EnvValue, null);
                return;
            }

            ConfigFileValues file = null;
            if (text == null || IsFreshConfig(text))
            {
                file = FirstConfig(() => Guid.NewGuid().ToString());
                _fileIdentity = file.IdentityToken;
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(_configPath));
                    File.WriteAllText(_configPath, RenderConfigText(file));
                    text = File.ReadAllText(_configPath);
                    LogInfo($"Created {_configPath} with a new identity token");
                }
                catch (Exception ex)
                {
                    LogWarning($"Could not create {_configPath}: {ex.Message}");
                }
            }
            else
            {
                try
                {
                    file = ParseConfigText(text);
                    text = EnsureIdentity(text, file);
                }
                catch (Exception ex)
                {
                    _configProblem = text;
                    LogWarning($"Could not read {_configPath} ({ex.Message}); fix the file and save it, the plugin re-reads it every few seconds");
                    file = new ConfigFileValues();
                }
            }

            _configText = text;
            _settings = ResolveSettings(file, EnvValue, null);
        }

        // A file without an IdentityToken gets this server's identity (a new one if it never
        // had one) written back. Returns the text now in the file.
        private string EnsureIdentity(string text, ConfigFileValues file)
        {
            if (!string.IsNullOrEmpty(file.IdentityToken))
            {
                _fileIdentity = file.IdentityToken;
                return text;
            }
            var generated = _fileIdentity == null;
            if (generated) _fileIdentity = Guid.NewGuid().ToString();
            file.IdentityToken = _fileIdentity;
            try
            {
                var updated = WithIdentity(text, _fileIdentity);
                // Replaced in one step, so a crash never leaves the token half written.
                var tmp = _configPath + ".tmp";
                File.WriteAllText(tmp, updated);
                File.Replace(tmp, _configPath, null);
                LogInfo(generated
                    ? $"No IdentityToken in {_configPath}; generated one and saved it"
                    : $"IdentityToken was removed from {_configPath}; putting this server's identity back");
                return updated;
            }
            catch (Exception ex)
            {
                LogWarning($"Could not save the IdentityToken in {_configPath} ({ex.Message}); using it for this run");
                return text;
            }
        }

        // Runs on the main thread every second: re-reads the config every few seconds and
        // starts any reconnect a closed socket asked for.
        private void Tick()
        {
            if (++_tickCount % ConfigCheckEverySeconds == 0)
            {
                try { CheckConfig(); }
                catch (Exception ex) { LogWarning($"Error checking {_configPath}: {ex.Message}"); }
            }

            var request = Interlocked.Exchange(ref _pendingReconnect, null);
            if (request == null || request.Generation != _generation) return;
            if (DateTime.UtcNow.Ticks < request.DueTicks)
            {
                Interlocked.CompareExchange(ref _pendingReconnect, request, null);
                return;
            }
            if (_shouldReconnect && !string.IsNullOrEmpty(_settings.RegistrationToken))
                StartConnection();
        }

        // Compares the whole text: a timestamp can miss a same-length edit. A file that is
        // missing, empty or unparseable mid-save keeps the current settings until next time.
        private void CheckConfig()
        {
            string text;
            try { text = ReadConfigText(_configPath); }
            catch { return; }
            if (text == _configText) { _configProblem = null; return; }

            ConfigFileValues file;
            if (!TryParseRunningConfig(text, out file))
            {
                if (_configProblem != (text ?? ""))
                    LogWarning(string.IsNullOrWhiteSpace(text)
                        ? $"{_configPath} is missing or empty; keeping the current settings. Reload the plugin to recreate it"
                        : $"Could not read {_configPath}; keeping the current settings and trying again in a few seconds");
                _configProblem = text ?? "";
                return;
            }

            _configProblem = null;
            _configText = EnsureIdentity(text, file);
            var previous = _settings;
            var next = ResolveSettings(file, EnvValue, previous);
            _settings = next;
            if (next.SameConnection(previous)) return;

            LogInfo($"{ConfigFileName} changed; reconnecting with the new settings (registration token {TokenSource(next)})");
            Reconnect();
        }

        // Drops the current socket and connects afresh, skipping whatever backoff a rejected
        // token had built up.
        private void Reconnect()
        {
            Interlocked.Increment(ref _generation);
            Interlocked.Exchange(ref _pendingReconnect, null);
            _connected = false;
            _protocolVersion = -1;
            _cts?.Cancel();
            try { _ws?.Dispose(); } catch { }
            _ws = null;
            _currentReconnectDelay = InitialReconnectDelay;
            _shouldReconnect = true;

            if (string.IsNullOrEmpty(_settings.RegistrationToken))
            {
                LogMissingToken();
                return;
            }
            LogInfo($"Connecting to {_settings.WsUrl}");
            StartConnection();
        }

        // Config problems stop the connection outright, so they must stand out in a busy
        // server console.
        private void LogBanner(params string[] lines)
        {
            const string rule = "*************************************************************************";
            var text = new StringBuilder();
            text.Append(rule);
            foreach (var line in lines)
                text.Append("\n  ").Append(line);
            text.Append('\n').Append(rule);
            try { PrintWarning("\n" + text); } catch { }
        }

        private void LogMissingToken()
        {
            LogBanner(
                "RegistrationToken not set, the server is not connected to Takaro.",
                $"Paste the registration token from Takaro into {_configPath}",
                "and save it. The plugin connects within a few seconds, no restart needed.");
        }

        private void LogRejected(string reason)
        {
            if (_settings.RegistrationFromEnv)
            {
                LogBanner(
                    $"Takaro rejected identify: {reason}.",
                    $"Check {EnvRegistrationToken} in the server's environment; it overrides",
                    $"{_configPath}. The environment is only read when the server starts.");
                return;
            }
            LogBanner(
                $"Takaro rejected identify: {reason}.",
                $"Check RegistrationToken in {_configPath}. Saving a corrected",
                "token reconnects within a few seconds, no restart needed.");
        }

        // --- WebSocket Connection ---

        // Main thread only.
        private void StartConnection()
        {
            var generation = Interlocked.Increment(ref _generation);
            var settings = _settings;
            _cts?.Cancel();
            var cts = new CancellationTokenSource();
            _cts = cts;
            var token = cts.Token;
            var ws = new ClientWebSocket();
            _ws = ws;

            Task.Run(async () =>
            {
                try
                {
                    await ws.ConnectAsync(new Uri(settings.WsUrl), token);
                    if (!IsCurrent(generation)) return;
                    LogInfo("WebSocket connected");
                    // Identify is the first frame this connector sends, the way every other
                    // Takaro connector in this repository does it. Waiting to be greeted
                    // instead means a peer that expects the client to speak first never
                    // hears from this server at all: the socket sits open and silent.
                    SendIdentify(ws, generation, settings);
                    await ReceiveLoop(ws, generation, settings, token);
                }
                catch (OperationCanceledException) { }
                catch (Exception ex)
                {
                    if (IsCurrent(generation))
                        LogWarning($"WebSocket connection failed: {ex.Message}");
                }
                finally
                {
                    try { ws.Dispose(); } catch { }
                    if (IsCurrent(generation))
                    {
                        _connected = false;
                        _protocolVersion = -1;
                        Interlocked.CompareExchange(ref _ws, null, ws);
                        if (_shouldReconnect && Volatile.Read(ref _authClosedGeneration) != generation)
                            ScheduleReconnect(generation);
                    }
                }
            });
        }

        private bool IsCurrent(int generation)
        {
            return Volatile.Read(ref _generation) == generation;
        }

        private async Task ReceiveLoop(ClientWebSocket ws, int generation, ConnectorSettings settings, CancellationToken token)
        {
            var buffer = new byte[8192];

            while (ws.State == WebSocketState.Open && !token.IsCancellationRequested)
            {
                var sb = new StringBuilder();
                WebSocketReceiveResult result;

                do
                {
                    result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), token);
                    sb.Append(Encoding.UTF8.GetString(buffer, 0, result.Count));
                } while (!result.EndOfMessage);

                if (!IsCurrent(generation)) break;

                if (result.MessageType == WebSocketMessageType.Close)
                {
                    var code = (int)(result.CloseStatus ?? WebSocketCloseStatus.NormalClosure);
                    var reason = result.CloseStatusDescription ?? "";
                    LogInfo($"WebSocket closed (code={code}, reason={reason})");

                    if ((code == 1008 || code == 4001 || code == 4003) && Volatile.Read(ref _retryableRejectGeneration) != generation)
                    {
                        Volatile.Write(ref _authClosedGeneration, generation);
                        LogRejected(string.IsNullOrEmpty(reason) ? $"connection closed with code {code}" : reason);
                    }
                    break;
                }

                var message = sb.ToString();
                if (_debug)
                    LogDebug($"WS RECV: {FrameSummary(message)}");

                try
                {
                    OnWsMessage(message, ws, generation, settings);
                }
                catch (Exception ex)
                {
                    LogWarning($"Error handling message: {ex.Message}");
                }
            }
        }

        // takaro:frames-begin
        private static string FrameSummary(string message)
        {
            try
            {
                var json = JObject.Parse(message);
                var type = json.Value<string>("type") ?? "";
                if (type != "request") return $"type={type}";
                return $"type=request action={json.Value<string>("action") ?? ""} " +
                    $"requestId={json.Value<string>("requestId") ?? ""}";
            }
            catch
            {
                return $"unparseable frame ({Encoding.UTF8.GetByteCount(message ?? "")} bytes)";
            }
        }
        // takaro:frames-end

        // takaro:identity-begin
        // A real Rust player's UserIDString is a SteamID64. NPCs (scientists, murderers, tunnel
        // dwellers) are BasePlayers too, with small numeric ids; sending one of those as a
        // Takaro player creates a bogus profile ("Scientist", steamId "67").
        private static bool IsSteamPlayerId(string id)
        {
            if (string.IsNullOrEmpty(id) || id.Length != 17 || !id.StartsWith("7656119", StringComparison.Ordinal))
                return false;
            foreach (var c in id)
                if (c < '0' || c > '9') return false;
            return true;
        }

        // Takaro matches players only on steamId / epicOnlineServicesId / xboxLiveId /
        // platformId and never derives steamId from platformId, so steamId is always sent
        // raw. Rust is Steam-only: no EOS or Xbox id exists, and those keys are omitted.
        private static string[][] IdentityFields(string steamId)
        {
            return new[]
            {
                new[] { "gameId", steamId },
                new[] { "steamId", steamId },
                new[] { "platformId", "steam:" + steamId },
            };
        }
        // takaro:identity-end

        private static bool IsRealPlayer(BasePlayer player)
        {
            return player != null && !player.IsNpc && IsSteamPlayerId(player.UserIDString);
        }

        private static JObject IdentityJson(string steamId, string name)
        {
            var json = new JObject();
            foreach (var field in IdentityFields(steamId))
                json[field[0]] = field[1];
            json["name"] = name ?? "";
            return json;
        }

        private void OnWsMessage(string message, ClientWebSocket ws, int generation, ConnectorSettings settings)
        {
            var json = JObject.Parse(message);
            var type = json.Value<string>("type") ?? "";

            switch (type)
            {
                case "connected":
                    // Takaro greets a fresh connection. Identify has already gone out when
                    // the socket opened, so this only does anything if the greeting beat it.
                    LogInfo("Received server hello");
                    SendIdentify(ws, generation, settings);
                    break;

                case "identifyResponse":
                    HandleIdentifyResponse(json, ws, generation);
                    break;

                case "eventAck":
                    if (TryParseEventAck(json, out var ackStream, out var upTo))
                        _events.Ack(ackStream, upTo);
                    break;

                case "eventError":
                    var eventErrorObj = json["error"] as JObject;
                    LogWarning($"Takaro rejected event seq={json["payload"]?["seq"]}: " +
                               $"{eventErrorObj?.Value<string>("code") ?? "error"}: {RedactJwt(eventErrorObj?.Value<string>("message") ?? "")}");
                    break;

                case "capabilitiesResponse":
                    var capabilitiesError = json["error"];
                    if (capabilitiesError != null && capabilitiesError.Type == JTokenType.Object)
                        LogWarning($"Takaro rejected the capability manifest: {IdentifyErrorText(capabilitiesError)}");
                    break;

                case "pong":
                    break;

                case "request":
                    HandleRequest(json, ws, generation);
                    break;

                case "error":
                    var errorMsg = json["payload"]?.Value<string>("message")
                                   ?? json.Value<string>("message")
                                   ?? "unknown";
                    var reqId = json.Value<string>("requestId");
                    LogWarning($"Server error: {RedactJwt(errorMsg)}" + (reqId != null ? $" (requestId={reqId})" : ""));
                    break;

                default:
                    LogWarning($"Unknown message type: {type}");
                    break;
            }
        }

        // --- Reconnection ---

        // Called from a socket task; the main-thread tick starts the connection when it is due.
        private void ScheduleReconnect(int generation)
        {
            if (!_shouldReconnect) return;

            var delaySec = _currentReconnectDelay / 1000.0f;
            LogInfo($"Reconnecting in {delaySec:F0}s...");

            Interlocked.Exchange(ref _pendingReconnect, new ReconnectRequest
            {
                Generation = generation,
                DueTicks = DateTime.UtcNow.Ticks + _currentReconnectDelay * TimeSpan.TicksPerMillisecond,
            });

            _currentReconnectDelay = Math.Min(
                (long)(_currentReconnectDelay * BackoffMultiplier),
                MaxReconnectDelay
            );
        }

        // --- Send Helpers ---

        private void WsSend(string message)
        {
            WsSendOn(_ws, message);
        }

        // Identify and responses go out on the socket they belong to, never on one a config
        // reconnect has put in its place.
        private void WsSendOn(ClientWebSocket ws, string message)
        {
            if (ws?.State != WebSocketState.Open) return;

            if (_debug && !message.Contains("\"type\":\"identify\""))
                LogDebug($"WS SEND: {message}");

            lock (_sendQueue)
            {
                _sendQueue.Enqueue(new OutgoingFrame { Socket = ws, Text = message, Token = _cts?.Token ?? CancellationToken.None });
                if (_sendDraining) return;
                _sendDraining = true;
            }
            _ = Task.Run(DrainSendQueue);
        }

        private void DrainSendQueue()
        {
            while (true)
            {
                OutgoingFrame next;
                lock (_sendQueue)
                {
                    if (_sendQueue.Count == 0)
                    {
                        _sendDraining = false;
                        return;
                    }
                    next = _sendQueue.Dequeue();
                }
                var ws = next.Socket;
                if (ws.State != WebSocketState.Open) continue;
                try
                {
                    ws.SendAsync(
                        new ArraySegment<byte>(Encoding.UTF8.GetBytes(next.Text)),
                        WebSocketMessageType.Text,
                        true,
                        next.Token
                    ).GetAwaiter().GetResult();
                }
                catch (Exception ex)
                {
                    LogWarning($"Send failed: {ex.Message}");
                }
            }
        }

        private JArray CurrentManifest()
        {
            return BuildManifest(GameProfileCapability(GameBuild(), _gameIcon), _mapCapability);
        }

        // The running game build as the server reports it (`version` prints it as Protocol).
        private static string GameBuild()
        {
            try { return Rust.Protocol.printable; }
            catch { return null; }
        }

        private void SendIdentify(ClientWebSocket ws, int generation, ConnectorSettings settings)
        {
            // At most once per connection: the socket opening and a server greeting both
            // lead here, and a second identify would look like a second session.
            if (Interlocked.Exchange(ref _identifySentGeneration, generation) == generation) return;

            var msg = new JObject
            {
                ["type"] = "identify",
                ["payload"] = BuildIdentifyPayload(settings.IdentityToken, settings.RegistrationToken, CurrentManifest(),
                    ConVar.Server.hostname, Version.ToString())
            };
            LogDebug("WS SEND identify (tokens redacted)");
            WsSendOn(ws, msg.ToString(Formatting.None));
        }

        private void HandleIdentifyResponse(JObject json, ClientWebSocket ws, int generation)
        {
            var reply = ParseIdentifyResponse(json);
            if (!reply.Accepted)
            {
                LogWarning($"Identify failed: {reply.Reason}");
                if (reply.Retryable)
                {
                    // Takaro closes the socket right behind this; the close then reconnects with backoff.
                    Volatile.Write(ref _retryableRejectGeneration, generation);
                    LogWarning("Takaro could not identify this server on its side; retrying with backoff");
                    return;
                }
                LogRejected(reply.Reason);
                return;
            }

            _currentReconnectDelay = InitialReconnectDelay;
            if (reply.GameServerId != null)
                LogInfo($"Identified successfully, server ID: {reply.GameServerId} (protocol {reply.ProtocolVersion})");
            else
                LogInfo($"Identified successfully (protocol {reply.ProtocolVersion})");

            // Events raised before this point were buffered. v1 resends everything Takaro has
            // not acked, in seq order; v0 has no acks, so the backlog goes out once.
            lock (_events.Lock)
            {
                if (!IsCurrent(generation)) return;
                _protocolVersion = reply.ProtocolVersion;
                _connected = true;
                var backlog = reply.ProtocolVersion >= 1 ? _events.Pending() : _events.TakeAll().Select(V0EventFrame).ToList();
                if (backlog.Count > 0)
                    LogInfo($"Sending {backlog.Count} buffered event(s)");
                foreach (var frame in backlog)
                    WsSendOn(ws, frame.ToString(Formatting.None));
            }
        }

        // Main thread: the map became available (or changed), so a v1 connection gets the
        // full manifest again. Before identify, the next identify carries it.
        private void SendCapabilities()
        {
            if (!_connected || _protocolVersion < 1) return;
            var msg = new JObject
            {
                ["type"] = "capabilities",
                ["requestId"] = Guid.NewGuid().ToString(),
                ["payload"] = new JObject { ["capabilities"] = CurrentManifest() }
            };
            LogInfo("Sending the updated capability manifest");
            WsSend(msg.ToString(Formatting.None));
        }

        private void SendResponse(ClientWebSocket ws, int protocolVersion, string requestId, JToken payload, ProtocolError error)
        {
            if (_debug)
                LogDebug($"WS SEND response (requestId={requestId})");

            WsSendOn(ws, ResponseFrame(protocolVersion, requestId, payload, error).ToString(Formatting.None));
        }

        // Game thread. Never touches the network itself: the frame is queued for the sender.
        private void SendGameEvent(string eventType, JObject data)
        {
            lock (_events.Lock)
            {
                if (!_connected)
                {
                    _events.Add(eventType, data, keep: true);
                    return;
                }
                var v1 = _protocolVersion >= 1;
                var frame = _events.Add(eventType, data, keep: v1);

                if (_debug)
                    LogDebug($"WS SEND gameEvent: {eventType}");

                WsSend((v1 ? frame : V0EventFrame(frame)).ToString(Formatting.None));
            }
        }

        // --- Request Handling ---

        private void HandleRequest(JObject json, ClientWebSocket ws, int generation)
        {
            var requestId = json.Value<string>("requestId");
            if (requestId == null)
            {
                LogWarning("Received request without requestId");
                return;
            }

            var protocol = Math.Max(0, _protocolVersion);
            var payload = json["payload"] as JObject ?? new JObject();
            var action = payload.Value<string>("action") ?? "";
            if (_debug)
                LogDebug($"Request: action={action}, requestId={requestId}");

            JObject args;
            var argsToken = payload["args"];
            try
            {
                if (argsToken != null && argsToken.Type == JTokenType.String)
                {
                    var argsStr = argsToken.Value<string>();
                    args = string.IsNullOrEmpty(argsStr) ? new JObject() : JObject.Parse(argsStr);
                }
                else if (argsToken != null && argsToken.Type == JTokenType.Object)
                {
                    args = (JObject)argsToken;
                }
                else
                {
                    args = new JObject();
                }
            }
            catch (Exception ex)
            {
                SendResponse(ws, protocol, requestId, null,
                    new ProtocolError(ErrorInvalidArgs, $"Invalid args JSON: {ex.Message}"));
                return;
            }

            if (action == "testReachability")
            {
                SendResponse(ws, protocol, requestId, new JObject { ["connectable"] = true, ["reason"] = null }, null);
                return;
            }

            // Answered here, off the game thread: the bytes are already in memory.
            if (action == "getGameIcon" && protocol >= 1)
            {
                var icon = _gameIcon;
                if (icon != null)
                {
                    LogInfo($"Serving the game icon (revision {icon.Revision}, {icon.Size} bytes)");
                    SendResponse(ws, protocol, requestId, GameIconResponse(icon), null);
                }
                else
                    SendResponse(ws, protocol, requestId, null, new ProtocolError(ErrorGameError, "the game icon could not be read"));
                return;
            }

            // v1 has no getMapInfo (the map capability replaces it); getMapTile changes shape.
            var handled = IsDeclaredAction(action)
                || action == "getMapTile"
                || (action == "getMapInfo" && protocol == 0);
            if (!handled)
            {
                SendResponse(ws, protocol, requestId, null,
                    new ProtocolError(ErrorUnsupported, $"Action not supported: {action}", $"Action not implemented: {action}"));
                return;
            }

            RunOnMainThread(ws, generation, protocol, requestId, action, args);
        }

        private void RunOnMainThread(ClientWebSocket ws, int generation, int protocol, string requestId, string action, JObject args)
        {
            NextTick(() =>
            {
                // A request from a connection that has since been replaced is dropped: its
                // socket is gone, and Takaro re-sends what it still wants on the new one.
                if (!IsCurrent(generation))
                {
                    LogInfo($"Dropped {action} from a replaced connection (requestId={requestId})");
                    return;
                }
                try
                {
                    var result = ExecuteAction(action, args, protocol);
                    SendResponse(ws, protocol, requestId, result, null);
                }
                catch (ProtocolError ex)
                {
                    LogWarning($"Action {action} failed: {ex.Code}: {ex.Message}");
                    SendResponse(ws, protocol, requestId, null, ex);
                }
                catch (Exception ex)
                {
                    LogWarning($"Action {action} failed: {ex.Message}");
                    SendResponse(ws, protocol, requestId, null, new ProtocolError(ErrorGameError, ex.Message));
                }
            });
        }

        // Null means "nothing to return": v1 answers null, v0 an empty object.
        private JToken ExecuteAction(string action, JObject args, int protocol)
        {
            switch (action)
            {
                case "getPlayer": return HandleGetPlayer(args);
                case "getPlayers": return HandleGetPlayers();
                case "getPlayerLocation": return HandleGetPlayerLocation(args, protocol);
                case "getPlayerInventory": return HandleGetPlayerInventory(args);
                case "listItems": return HandleListItems();
                case "listEntities": return HandleListEntities();
                case "listLocations": return HandleListLocations();
                case "executeConsoleCommand": return HandleExecuteConsoleCommand(args);
                case "sendMessage": HandleSendMessage(args); return null;
                case "giveItem": HandleGiveItem(args); return null;
                case "teleportPlayer": HandleTeleportPlayer(args); return null;
                case "kickPlayer": HandleKickPlayer(args); return null;
                case "banPlayer": HandleBanPlayer(args); return null;
                case "unbanPlayer": HandleUnbanPlayer(args); return null;
                case "listBans": return HandleListBans();
                case "shutdown": HandleShutdown(); return null;
                case "getMapInfo": return HandleGetMapInfo();
                case "getMapTile": return protocol >= 1 ? HandleGetMapTileV1(args) : HandleGetMapTile(args);
                default: throw new ProtocolError(ErrorUnsupported, $"Action not supported: {action}", $"Unknown action: {action}");
            }
        }

        // --- Action Handlers ---

        // Takaro sends the player either flat ({gameId}) or nested ({player:{gameId}}),
        // depending on the route, so every handler accepts both.
        private static string GameIdFrom(JObject args)
        {
            var nested = (args["player"] as JObject)?.Value<string>("gameId");
            return string.IsNullOrEmpty(nested) ? args.Value<string>("gameId") : nested;
        }

        private BasePlayer FindPlayerByGameId(string gameId)
        {
            if (string.IsNullOrEmpty(gameId)) return null;

            if (ulong.TryParse(gameId, out var steamId))
                return BasePlayer.FindByID(steamId) ?? BasePlayer.FindSleeping(steamId);

            return null;
        }

        private JObject PlayerToJson(BasePlayer player)
        {
            var json = IdentityJson(player.UserIDString, player.displayName);
            json["ip"] = player.net?.connection?.ipaddress?.Split(':')[0] ?? "";
            json["ping"] = player.IsConnected ? Network.Net.sv.GetAveragePing(player.net.connection) : 0;
            return json;
        }

        private JToken HandleGetPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var player = FindPlayerByGameId(gameId);
            return player != null ? (JToken)PlayerToJson(player) : JValue.CreateNull();
        }

        private JToken HandleGetPlayers()
        {
            var arr = new JArray();
            foreach (var player in BasePlayer.activePlayerList)
                if (IsRealPlayer(player)) arr.Add(PlayerToJson(player));
            return arr;
        }

        private JToken HandleGetPlayerLocation(JObject args, int protocol)
        {
            var gameId = GameIdFrom(args);
            var player = FindPlayerByGameId(gameId);

            if (player != null)
            {
                var pos = player.transform.position;
                _lastPosition[gameId] = pos;
                return PlayerLocationPayload(pos.x, pos.y, pos.z, protocol);
            }

            if (_lastPosition.TryGetValue(gameId, out var cached))
                return PlayerLocationPayload(cached.x, cached.y, cached.z, protocol);

            return UnknownPlayerLocation(protocol);
        }

        private JToken HandleGetPlayerInventory(JObject args)
        {
            var gameId = GameIdFrom(args);
            var player = FindPlayerByGameId(gameId);
            if (player == null) return JValue.CreateNull();

            var items = new JArray();
            var containers = new[] {
                player.inventory.containerMain,
                player.inventory.containerBelt,
                player.inventory.containerWear
            };

            foreach (var container in containers)
            {
                if (container == null) continue;
                foreach (var item in container.itemList)
                {
                    items.Add(new JObject
                    {
                        ["code"] = item.info.shortname,
                        ["name"] = item.info.displayName.english,
                        ["amount"] = item.amount,
                        ["quality"] = ""
                    });
                }
            }
            return items;
        }

        private JToken HandleListItems()
        {
            var arr = new JArray();
            foreach (var def in ItemManager.itemList)
            {
                arr.Add(new JObject
                {
                    ["code"] = def.shortname,
                    ["name"] = def.displayName.english,
                    ["description"] = def.displayDescription?.english ?? ""
                });
            }
            return arr;
        }

        private JToken HandleListEntities()
        {
            var seen = new HashSet<string>();
            var arr = new JArray();

            foreach (var path in GameManifest.Current.entities)
            {
                if (path.Contains("corpse") || path.Contains("ragdoll") || path.Contains("_dead")) continue;

                var prefab = GameManager.server.FindPrefab(path);
                if (prefab == null) continue;

                // Animals moved to BaseNPC2 (bear, wolf2, chicken, ...); BaseNpc is left with
                // the unused shark only, so both are listed, plus the swimming SimpleShark.
                if (prefab.GetComponent<BaseNpc>() == null && prefab.GetComponent<Rust.Ai.Gen2.BaseNPC2>() == null
                    && prefab.GetComponent<NPCPlayer>() == null && prefab.GetComponent<SimpleShark>() == null) continue;

                var shortName = prefab.name;
                if (string.IsNullOrEmpty(shortName) || !seen.Add(shortName)) continue;

                arr.Add(new JObject
                {
                    ["code"] = shortName,
                    ["name"] = EntityNames.EntityDisplayName(shortName),
                    ["description"] = ""
                });
            }

            return arr;
        }

        // What a console command looks like in the log: the verb, and how many arguments
        // came with it. `console: help` is unchanged; `say <anything>` never appears.
        private static string CommandSummary(string command)
        {
            var parts = (command ?? "").Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return "";
            return parts.Length == 1 ? parts[0] : parts[0] + " (" + (parts.Length - 1) + " args)";
        }

        // takaro:names-begin
        // Entity prefabs have no localised display name the way items do -- `bear`,
        // `scientistnpc_heavy`, `wolf2` is all the server knows them by. Opening the
        // separators and capitalising put "Scientistnpc Heavy" and "Missionprovider
        // Floatingcity A" in the one field Takaro shows a
        // human: a formatted dev code, not a name.
        //
        // The table is the prefab list a real server returned; anything outside it goes
        // through the rules below it. Nothing in this region touches Rust, Carbon or
        // Unity, and the markers are what `tests/names/run.sh` lifts out to compile and
        // run it on its own -- the behaviour is proven there, not grepped for.
        private static class EntityNames
        {
            private static readonly Dictionary<string, string> Table =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "scientistnpc_arena", "Arena Scientist" },
                    { "scientistnpc_bradley", "Bradley Scientist" },
                    { "scientistnpc_bradley_heavy", "Bradley Heavy Scientist" },
                    { "scientistnpc_cargo", "Cargo Ship Scientist" },
                    { "scientistnpc_cargo_turret_any", "Cargo Ship Turret Scientist" },
                    { "scientistnpc_cargo_turret_lr300", "Cargo Ship Turret Scientist (LR-300)" },
                    { "scientistnpc_ch47_gunner", "Chinook Gunner Scientist" },
                    { "scientistnpc_excavator", "Excavator Scientist" },
                    { "scientistnpc_full_any", "Scientist" },
                    { "scientistnpc_full_lr300", "Scientist (LR-300)" },
                    { "scientistnpc_full_mp5", "Scientist (MP5)" },
                    { "scientistnpc_full_pistol", "Scientist (Pistol)" },
                    { "scientistnpc_full_shotgun", "Scientist (Shotgun)" },
                    { "scientistnpc_heavy", "Heavy Scientist" },
                    { "scientistnpc_junkpile_pistol", "Junkpile Scientist" },
                    { "scientistnpc_oilrig", "Oil Rig Scientist" },
                    { "scientistnpc_outbreak", "Outbreak Scientist" },
                    { "scientistnpc_patrol", "Patrol Scientist" },
                    { "scientistnpc_patrol_arctic", "Arctic Patrol Scientist" },
                    { "scientistnpc_peacekeeper", "Peacekeeper Scientist" },
                    { "scientistnpc_ptboat", "Patrol Boat Scientist" },
                    { "scientistnpc_rhib", "RHIB Scientist" },
                    { "scientistnpc_roam", "Roaming Scientist" },
                    { "scientistnpc_roam_nvg_variant", "Roaming Scientist (Night Vision)" },
                    { "scientistnpc_roamtethered", "Tethered Roaming Scientist" },
                    { "npc_bandit_guard", "Bandit Guard" },
                    { "npc_tunneldweller", "Tunnel Dweller" },
                    { "npc_tunneldwellerspawned", "Tunnel Dweller (Spawned)" },
                    { "npc_underwaterdweller", "Underwater Dweller" },
                    { "npcplayertest", "NPC Player (Test)" },
                    { "polarbear", "Polar Bear" },
                    { "bear", "Bear" },
                    { "bear_tutorial", "Bear (Tutorial)" },
                    { "boar", "Boar" },
                    { "chicken", "Chicken" },
                    { "chicken.tutorial", "Chicken (Tutorial)" },
                    { "stag", "Stag" },
                    { "wolf", "Wolf" },
                    { "wolf2", "Wolf" },
                    { "ridablehorse", "Horse" },
                    { "ridablehorse2", "Horse" },
                    { "simpleshark", "Shark" },
                    { "shark_unused", "Shark (Unused)" },
                    { "zombie", "Zombie" },
                    { "bull", "Bull" },
                    { "calf", "Calf" },
                    { "calfmale", "Calf (Male)" },
                    { "cow", "Cow" },
                    { "lamb", "Lamb" },
                    { "sheep", "Sheep" },
                    { "crab_single", "Crab" },
                    { "crabs", "Crabs" },
                    { "crocodile", "Crocodile" },
                    { "frog", "Frog" },
                    { "jellyfish", "Jellyfish" },
                    { "panther", "Panther" },
                    { "tiger", "Tiger" },
                    { "rabbit", "Rabbit" },
                    { "squirrel", "Squirrel" },
                    { "seaturtle", "Sea Turtle" },
                    { "scientist2", "Scientist" },
                    { "scientist2.heavy", "Heavy Scientist" },
                    { "scientist2.shotgun", "Scientist (Shotgun)" },
                    { "scarecrow", "Scarecrow" },
                    { "scarecrow_dungeon", "Scarecrow (Dungeon)" },
                    { "scarecrow_dungeonnoroam", "Scarecrow (Dungeon, Stationary)" },
                    { "gingerbread_dungeon", "Gingerbread Man (Dungeon)" },
                    { "gingerbread_meleedungeon", "Gingerbread Man (Melee Dungeon)" },
                    { "frankensteinpet", "Frankenstein Pet" },
                    { "apartment_vendor", "Apartment Vendor" },
                    { "apartment_security", "Apartment Security" },
                    { "farm_access_guard", "Farm Access Guard" },
                    { "bandit_conversationalist", "Bandit Conversationalist" },
                    { "bandit_shopkeeper", "Bandit Shopkeeper" },
                    { "bandit_shopkeeper_sitting", "Bandit Shopkeeper (Sitting)" },
                    { "boat_shopkeeper", "Boat Shopkeeper" },
                    { "stables_shopkeeper", "Stables Shopkeeper" },
                    { "livestockvendor_stables", "Stables Livestock Vendor" },
                    { "waterwell_shopkeeper", "Water Well Shopkeeper" },
                    { "missionprovider_bandit_a", "Bandit Mission Provider A" },
                    { "missionprovider_bandit_b", "Bandit Mission Provider B" },
                    { "missionprovider_fishing_a", "Fishing Mission Provider A" },
                    { "missionprovider_fishing_b", "Fishing Mission Provider B" },
                    { "missionprovider_floatingcity_a", "Floating City Mission Provider A" },
                    { "missionprovider_generic_a", "Generic Mission Provider A" },
                    { "missionprovider_outpost_a", "Outpost Mission Provider A" },
                    { "missionprovider_outpost_b", "Outpost Mission Provider B" },
                    { "missionprovider_stables_a", "Stables Mission Provider A" },
                    { "missionprovider_stables_b", "Stables Mission Provider B" },
                    { "missionprovider_test", "Mission Provider (Test)" },
                    { "missionprovider_tutorial", "Mission Provider (Tutorial)" }
                };

            // Prefab words that are several words glued together, or an abbreviation that
            // capitalising alone would mangle into `Lr300`, `Ch47`, `Mp5`.
            private static readonly Dictionary<string, string> Glued =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "scientistnpc", "Scientist" },
                    { "tunneldweller", "Tunnel Dweller" },
                    { "underwaterdweller", "Underwater Dweller" },
                    { "polarbear", "Polar Bear" },
                    { "ridablehorse", "Horse" },
                    { "simpleshark", "Shark" },
                    { "waterwell", "Water Well" },
                    { "missionprovider", "Mission Provider" },
                    { "meleedungeon", "Melee Dungeon" },
                    { "dungeonnoroam", "Dungeon, Stationary" },
                    { "floatingcity", "Floating City" },
                    { "lr300", "LR-300" },
                    { "mp5", "MP5" },
                    { "rhib", "RHIB" },
                    { "ch47", "CH-47" },
                    { "nvg", "Night Vision" },
                    { "ptboat", "Patrol Boat" },
                    { "oilrig", "Oil Rig" },
                    { "npc", "NPC" }
                };

            // Not what the thing is but which copy of it, so these become a parenthesised
            // suffix rather than a word in the middle of the name.
            private static readonly Dictionary<string, string> Variants =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    { "tutorial", "Tutorial" },
                    { "unused", "Unused" },
                    { "test", "Test" },
                    { "spawned", "Spawned" }
                };

            private static readonly char[] Separators = { '_', '-', '.' };

            public static string EntityDisplayName(string shortName)
            {
                if (string.IsNullOrEmpty(shortName)) return shortName;

                string known;
                if (Table.TryGetValue(shortName, out known)) return known;

                var parts = shortName.Split(Separators, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) return shortName;

                // `missionprovider_bandit_a` is the bandit camp's mission provider, variant
                // A: the role belongs between the place and the variant, not at the front.
                var provider = parts[0].Equals("missionprovider", StringComparison.OrdinalIgnoreCase);
                var words = new List<string>(parts.Length);
                var suffixes = new List<string>();

                for (var i = provider ? 1 : 0; i < parts.Length; i++)
                {
                    string variant;
                    if (Variants.TryGetValue(parts[i], out variant))
                    {
                        suffixes.Add(variant);
                        continue;
                    }
                    words.Add(Word(parts[i]));
                }
                if (provider) words.Insert(words.Count > 0 ? 1 : 0, "Mission Provider");

                var name = string.Join(" ", words.ToArray());
                if (suffixes.Count > 0) name += " (" + string.Join(", ", suffixes.ToArray()) + ")";
                return name.Length == 0 ? shortName : name;
            }

            // One prefab word. A lone letter is a variant label (`_a`, `_b`); a trailing
            // copy number is dropped, because `wolf2` is the same animal as `wolf`.
            private static string Word(string part)
            {
                string glued;
                if (Glued.TryGetValue(part, out glued)) return glued;
                if (part.Length == 1) return part.ToUpperInvariant();

                var end = part.Length;
                while (end > 0 && char.IsDigit(part[end - 1])) end--;
                if (end > 0 && end < part.Length && char.IsLetter(part[end - 1]))
                {
                    var stem = part.Substring(0, end);
                    if (Glued.TryGetValue(stem, out glued)) return glued;
                    part = stem;
                }
                return char.ToUpperInvariant(part[0]) + part.Substring(1);
            }
        }
        // takaro:names-end

        // --- World map (getMapInfo / getMapTile) ---
        //
        // The map is the game's own render (MapImageRenderer, the code behind world.rendermap
        // and the Rust+ map), drawn once per world on the main thread and kept as a PNG in the
        // data directory, so later starts and reloads only decode it. Tiles are cut from that
        // raster on the main thread, where every action already runs.

        private const string MapCachePrefix = "TakaroConnector_map_";
        private const int MapTileCacheSize = 1024;

        private MapRaster _mapRaster;
        private string _mapStatus = "the world has not loaded yet";
        private readonly Dictionary<long, string> _mapTiles = new Dictionary<long, string>();
        private readonly Queue<long> _mapTileOrder = new Queue<long>();

        private void OnServerInitialized()
        {
            // Draw the map before anyone asks for it, so the first getMapInfo is not the one
            // that pays for the render.
            timer.Once(2f, () =>
            {
                try { EnsureMapRaster(); }
                catch (Exception ex) { LogWarning($"World map: {ex.Message}"); }
            });
        }

        private MapRaster EnsureMapRaster()
        {
            if (_mapRaster != null) return _mapRaster;

            var terrainSize = TerrainMeta.Size.x;
            if (global::World.Size == 0 || TerrainMeta.HeightMap == null || TerrainTexturing.Instance == null || !(terrainSize > 0f))
            {
                _mapStatus = "the world has not loaded yet";
                return null;
            }

            var key = MapWorldKey(global::World.Size, global::World.Seed, global::World.MapFileName, global::World.Checksum);
            var path = Path.Combine(Interface.Oxide.DataDirectory, MapCachePrefix + key + ".png");
            byte[] png = null;
            var source = "cache";
            if (File.Exists(path))
            {
                try { png = File.ReadAllBytes(path); }
                catch (Exception ex) { LogWarning($"World map: could not read {path}: {ex.Message}"); }
            }

            if (png == null)
            {
                var watch = System.Diagnostics.Stopwatch.StartNew();
                png = MapImageRenderer.Render(out _, out _, out _, MapRenderScale(terrainSize), lossy: false, transparent: false, oceanMargin: 0);
                if (png == null)
                {
                    _mapStatus = "the game could not render the map yet";
                    return null;
                }
                source = $"rendered in {watch.ElapsedMilliseconds} ms";
                try
                {
                    Directory.CreateDirectory(Interface.Oxide.DataDirectory);
                    foreach (var old in Directory.GetFiles(Interface.Oxide.DataDirectory, MapCachePrefix + "*.png"))
                        File.Delete(old);
                    File.WriteAllBytes(path, png);
                }
                catch (Exception ex)
                {
                    LogWarning($"World map: could not cache the render at {path}: {ex.Message}");
                }
            }

            var texture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
            try
            {
                if (!texture.LoadImage(png))
                {
                    _mapStatus = "the map render could not be decoded";
                    try { File.Delete(path); } catch { }
                    return null;
                }
                var pixels = texture.GetPixels32();
                int width = texture.width, height = texture.height;
                var rgb = new byte[width * height * 3];
                for (var i = 0; i < pixels.Length; i++)
                {
                    rgb[i * 3] = pixels[i].r;
                    rgb[i * 3 + 1] = pixels[i].g;
                    rgb[i * 3 + 2] = pixels[i].b;
                }
                // The game takes its background (open sea) from the corner pixel too.
                var corner = pixels[0];
                _mapRaster = new MapRaster(rgb, width, height, terrainSize,
                    TerrainMeta.Position.x, TerrainMeta.Position.z, width / terrainSize,
                    corner.r, corner.g, corner.b, key);
            }
            finally
            {
                UnityEngine.Object.Destroy(texture);
            }

            _mapTiles.Clear();
            _mapTileOrder.Clear();
            _mapStatus = "ready";
            LogInfo($"World map ready: {_mapRaster.Width}x{_mapRaster.Height} px for a {terrainSize:0} m world ({source}, key {key})");
            _mapCapability = MapCapability(_mapRaster);
            SendCapabilities();
            return _mapRaster;
        }

        private JToken HandleGetMapInfo()
        {
            var raster = EnsureMapRaster();
            return MapInfo(raster != null, raster?.WorldSize ?? global::World.Size);
        }

        private JToken HandleGetMapTile(JObject args)
        {
            var problem = ParseMapTileArgs(args, out var x, out var y, out var zoom);
            if (problem != null) throw new ProtocolError(ErrorInvalidArgs, problem, $"invalid_args: {problem}");

            var raster = EnsureMapRaster();
            if (raster == null)
                throw new ProtocolError(ErrorNotFound, $"the Rust world map is not ready: {_mapStatus}", $"map_not_ready: the Rust world map is not ready: {_mapStatus}");
            if (!MapTileInRange(raster.WorldSize, zoom, x, y))
                throw new ProtocolError(ErrorNotFound, $"map tile {zoom}/{x}/{y} lies outside the Rust world", $"tile_out_of_range: map tile {zoom}/{x}/{y} lies outside the Rust world");

            // v0 keys keep bit 63 clear; v1 keys set it, so both layouts share one cache.
            var cacheKey = ((long)zoom << 56) | (((long)x & 0xFFFFFFF) << 28) | ((long)y & 0xFFFFFFF);
            return new JValue(MapTilePng(cacheKey, () => RenderMapTileRgb(raster, zoom, x, y)));
        }

        private JToken HandleGetMapTileV1(JObject args)
        {
            var raster = EnsureMapRaster();
            if (raster == null) throw new ProtocolError(ErrorNotFound, $"the Rust world map is not ready: {_mapStatus}");
            var problem = ParseMapTileV1Args(args, MapV1Side(raster), out var zoom, out var x, out var y);
            if (problem != null) throw new ProtocolError(ErrorInvalidArgs, problem);

            var cacheKey = long.MinValue | ((long)zoom << 56) | (((long)x & 0xFFFFFFF) << 28) | ((long)y & 0xFFFFFFF);
            return new JObject
            {
                ["mimeType"] = "image/png",
                ["data"] = MapTilePng(cacheKey, () => RenderMapTileV1Rgb(raster, zoom, x, y)),
            };
        }

        // Base64 PNG of one tile, from the cache or rendered now.
        private string MapTilePng(long cacheKey, Func<byte[]> render)
        {
            if (_mapTiles.TryGetValue(cacheKey, out var cached)) return cached;

            var rgb = render();
            var texture = new Texture2D(MapTilePixels, MapTilePixels, TextureFormat.RGB24, false);
            string encoded;
            try
            {
                texture.LoadRawTextureData(rgb);
                texture.Apply(false);
                encoded = Convert.ToBase64String(texture.EncodeToPNG());
            }
            finally
            {
                UnityEngine.Object.Destroy(texture);
            }

            _mapTiles[cacheKey] = encoded;
            _mapTileOrder.Enqueue(cacheKey);
            while (_mapTileOrder.Count > MapTileCacheSize)
                _mapTiles.Remove(_mapTileOrder.Dequeue());
            return encoded;
        }

        private JToken HandleListLocations()
        {
            var arr = new JArray();
            if (TerrainMeta.Path?.Monuments != null)
            {
                foreach (var monument in TerrainMeta.Path.Monuments)
                {
                    if (monument == null) continue;
                    var pos = monument.transform.position;
                    arr.Add(new JObject
                    {
                        ["name"] = monument.displayPhrase?.english ?? monument.name,
                        ["code"] = monument.name,
                        ["position"] = new JObject
                        {
                            ["x"] = pos.x,
                            ["y"] = pos.y,
                            ["z"] = pos.z
                        }
                    });
                }
            }
            return arr;
        }

        private JToken HandleExecuteConsoleCommand(JObject args)
        {
            var command = args.Value<string>("command") ?? "";
            // Rust's console prints nothing for most commands it is handed (`say` among
            // them), so this line is the only record an operator has of what Takaro ran on
            // their server -- and the only thing outside the connector that shows a console
            // round trip happened at all. Only the verb goes in it: the arguments are
            // whatever Takaro was asked to run, up to and including a password an admin
            // typed, and this log is read by anyone who can read the server console.
            LogInfo($"console: {CommandSummary(command)}");
            try
            {
                var result = ConsoleSystem.Run(ConsoleSystem.Option.Server, command) ?? "";
                // A command that throws does not throw here: Rust hands back its message
                // as "Error: <command> - <message>" in place of the output.
                var failed = result.StartsWith("Error: ", StringComparison.Ordinal);
                return new JObject
                {
                    ["success"] = !failed,
                    ["rawResult"] = result,
                    ["errorMessage"] = failed ? result : ""
                };
            }
            catch (Exception ex)
            {
                return new JObject
                {
                    ["success"] = false,
                    ["rawResult"] = "",
                    ["errorMessage"] = ex.Message
                };
            }
        }

        private void HandleSendMessage(JObject args)
        {
            var opts = args["opts"] as JObject;
            var recipient = opts?["recipient"] as JObject;
            var recipientGameId = recipient?.Value<string>("gameId");
            var senderName = ResolveServerChatName(opts?["senderNameOverride"]?.Type == JTokenType.String
                ? opts.Value<string>("senderNameOverride") : null, _settings.ServerChatName);
            var text = args.Value<string>("message") ?? "";
            // A client command, not chat said on the server: OnPlayerChat never runs for it,
            // so it cannot come back to Takaro as a chat-message event, name or not.
            string command;
            var commandArgs = ServerChatCommand(senderName, text, out command);

            if (!string.IsNullOrEmpty(recipientGameId))
            {
                var player = FindPlayerByGameId(recipientGameId);
                if (player != null && player.net?.connection != null)
                    player.SendConsoleCommand(command, commandArgs);
            }
            else
            {
                // Same reasoning as the console audit line above: a broadcast leaves no
                // trace in Rust's own console, so nothing on the server would ever show
                // that Takaro said something to everybody.
                LogInfo(senderName == null ? $"broadcast: {text}" : $"broadcast as {senderName}: {text}");
                ConsoleNetwork.BroadcastToAllClients(command, commandArgs);
            }
        }

        private void HandleGiveItem(JObject args)
        {
            var gameId = GameIdFrom(args);
            var itemCode = args.Value<string>("item");
            var amount = args.Value<int?>("amount") ?? 1;

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new ProtocolError(ErrorNotFound, "Player not found");

            var itemDef = ItemManager.FindItemDefinition(itemCode);
            if (itemDef == null) throw new ProtocolError(ErrorNotFound, $"Item not found: {itemCode}");

            var item = ItemManager.Create(itemDef, amount);
            if (!player.inventory.GiveItem(item))
            {
                item.Drop(player.transform.position, Vector3.up);
            }
        }

        private void HandleTeleportPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var x = args.Value<float?>("x") ?? 0;
            var y = args.Value<float?>("y") ?? 0;
            var z = args.Value<float?>("z") ?? 0;

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new ProtocolError(ErrorNotFound, "Player not found");

            player.Teleport(new Vector3(x, y, z));
        }

        private void HandleKickPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var reason = args.Value<string>("reason") ?? "";

            var player = FindPlayerByGameId(gameId);
            if (player == null) throw new ProtocolError(ErrorNotFound, "Player not found");

            player.Kick(reason);
        }

        private void HandleBanPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            var reason = args.Value<string>("reason") ?? "";
            var expiresAt = args.Value<string>("expiresAt");

            if (string.IsNullOrEmpty(gameId)) throw new ProtocolError(ErrorInvalidArgs, "gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new ProtocolError(ErrorInvalidArgs, "Invalid gameId");

            var player = FindPlayerByGameId(gameId);
            var name = player?.displayName ?? KnownPlayerName(steamId) ?? gameId;

            DateTimeOffset expiry;
            if (TryParseExpiry(expiresAt, out expiry))
            {
                // Timed ban: keep it in our own data file, native ban list has no expiry.
                ServerUsers.Remove(steamId);
                ServerUsers.Save();

                _timedBans[steamId] = new TimedBan
                {
                    SteamId = gameId,
                    Name = name,
                    Reason = reason,
                    ExpiresAt = ToIso8601(expiry)
                };
                SaveTimedBans();
                LogInfo($"Timed ban for {gameId} until {ToIso8601(expiry)}");
            }
            else
            {
                if (_timedBans.Remove(steamId)) SaveTimedBans();

                ServerUsers.Set(steamId, ServerUsers.UserGroup.Banned, name, reason);
                ServerUsers.Save();
            }

            player?.Kick($"Banned: {reason}");
        }

        private void HandleUnbanPlayer(JObject args)
        {
            var gameId = GameIdFrom(args);
            if (string.IsNullOrEmpty(gameId)) throw new ProtocolError(ErrorInvalidArgs, "gameId required");
            if (!ulong.TryParse(gameId, out var steamId)) throw new ProtocolError(ErrorInvalidArgs, "Invalid gameId");

            if (_timedBans.Remove(steamId)) SaveTimedBans();

            ServerUsers.Remove(steamId);
            ServerUsers.Save();
        }

        // Takaro resolves every listed ban to a player through the same platform-id match as
        // events; a ban entry with only a gameId cannot be matched to a player Takaro has not
        // already seen on this server.
        private static JObject BanPlayerJson(string id, string name)
        {
            if (!IsSteamPlayerId(id)) return new JObject { ["gameId"] = id ?? "", ["name"] = name ?? "" };

            // Takaro copies a ban entry's name onto the matched player, so an offline ban that
            // stored the SteamID as its name renamed that player to the number.
            if (string.IsNullOrEmpty(name) || name == id)
                name = KnownPlayerName(ulong.Parse(id)) ?? name;
            return IdentityJson(id, name);
        }

        // Last name Rust saved for a player who is neither online nor sleeping.
        private static string KnownPlayerName(ulong steamId)
        {
            try
            {
                var name = SingletonComponent<ServerMgr>.Instance?.persistance?.GetPlayerName(steamId);
                return string.IsNullOrEmpty(name) ? null : name;
            }
            catch (Exception)
            {
                return null;
            }
        }

        private JToken HandleListBans()
        {
            PruneExpiredBans();

            var arr = new JArray();
            var bans = ServerUsers.GetAll(ServerUsers.UserGroup.Banned);
            foreach (var ban in bans)
            {
                arr.Add(new JObject
                {
                    ["player"] = BanPlayerJson(ban.steamid.ToString(), ban.username),
                    ["reason"] = ban.notes ?? "",
                    ["expiresAt"] = null
                });
            }

            foreach (var ban in _timedBans.Values)
            {
                arr.Add(new JObject
                {
                    ["player"] = BanPlayerJson(ban.SteamId, ban.Name),
                    ["reason"] = ban.Reason ?? "",
                    ["expiresAt"] = ban.ExpiresAt
                });
            }

            return arr;
        }

        // --- Timed Ban Storage & Enforcement ---

        private static bool TryParseExpiry(string expiresAt, out DateTimeOffset expiry)
        {
            expiry = default(DateTimeOffset);
            if (string.IsNullOrEmpty(expiresAt)) return false;

            DateTimeOffset parsed;
            if (!DateTimeOffset.TryParse(
                    expiresAt,
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AllowWhiteSpaces | DateTimeStyles.AssumeUniversal,
                    out parsed))
            {
                return false;
            }

            expiry = parsed.ToUniversalTime();
            return expiry > DateTimeOffset.UtcNow;
        }

        private static string ToIso8601(DateTimeOffset value) =>
            value.ToUniversalTime().ToString("o", CultureInfo.InvariantCulture);

        private void LoadTimedBans()
        {
            _timedBans.Clear();
            try
            {
                var stored = Interface.Oxide.DataFileSystem
                    .ReadObject<Dictionary<string, TimedBan>>(BanDataFile);
                if (stored == null) return;

                foreach (var entry in stored)
                {
                    ulong steamId;
                    if (entry.Value == null || !ulong.TryParse(entry.Key, out steamId)) continue;
                    if (string.IsNullOrEmpty(entry.Value.SteamId)) entry.Value.SteamId = entry.Key;
                    _timedBans[steamId] = entry.Value;
                }
            }
            catch (Exception ex)
            {
                LogWarning($"Could not read timed ban data: {ex.Message}");
            }

            PruneExpiredBans();
        }

        private void SaveTimedBans()
        {
            try
            {
                var toStore = new Dictionary<string, TimedBan>();
                foreach (var entry in _timedBans) toStore[entry.Key.ToString()] = entry.Value;
                Interface.Oxide.DataFileSystem.WriteObject(BanDataFile, toStore);
            }
            catch (Exception ex)
            {
                LogWarning($"Could not write timed ban data: {ex.Message}");
            }
        }

        /// <summary>Drops timed bans whose expiry has passed. Returns true if anything changed.</summary>
        private bool PruneExpiredBans()
        {
            if (_timedBans.Count == 0) return false;

            var now = DateTimeOffset.UtcNow;
            var expired = new List<ulong>();
            foreach (var entry in _timedBans)
            {
                DateTimeOffset expiry = default(DateTimeOffset);
                var parseable = !string.IsNullOrEmpty(entry.Value?.ExpiresAt)
                    && DateTimeOffset.TryParse(
                        entry.Value.ExpiresAt,
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AllowWhiteSpaces | DateTimeStyles.AssumeUniversal,
                        out expiry);

                // An unreadable expiry would never lift, so drop it rather than ban forever.
                if (!parseable || expiry.ToUniversalTime() <= now) expired.Add(entry.Key);
            }

            if (expired.Count == 0) return false;

            foreach (var steamId in expired)
            {
                _timedBans.Remove(steamId);
                LogInfo($"Timed ban for {steamId} expired, lifted");
            }
            SaveTimedBans();
            return true;
        }

        private object CanUserLogin(string name, string id, string ip)
        {
            ulong steamId;
            if (!ulong.TryParse(id, out steamId)) return null;

            TimedBan ban;
            if (!_timedBans.TryGetValue(steamId, out ban)) return null;

            PruneExpiredBans();
            if (!_timedBans.TryGetValue(steamId, out ban)) return null;

            return string.IsNullOrEmpty(ban.Reason) ? "Banned" : $"Banned: {ban.Reason}";
        }

        // The response is sent after this returns, so quitting here would kill the
        // process before Takaro hears back. Quit a moment later instead.
        private void HandleShutdown()
        {
            timer.Once(2f, () => ConsoleSystem.Run(ConsoleSystem.Option.Server, "quit"));
        }

        // --- Game Event Hooks ---

        private void OnPlayerConnected(BasePlayer player)
        {
            if (!IsRealPlayer(player)) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-connected", data);
        }

        private void OnPlayerDisconnected(BasePlayer player, string reason)
        {
            if (!IsRealPlayer(player)) return;

            _lastPosition[player.UserIDString] = player.transform.position;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };
            SendGameEvent("player-disconnected", data);
        }

        private object OnPlayerChat(BasePlayer player, string message, ConVar.Chat.ChatChannel channel)
        {
            if (!IsRealPlayer(player)) return null;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player),
                ["channel"] = channel.ToString().ToLower(),
                ["msg"] = message
            };
            SendGameEvent("chat-message", data);
            return null;
        }

        private void OnPlayerDeath(BasePlayer player, object info)
        {
            if (!IsRealPlayer(player)) return;

            var data = new JObject
            {
                ["player"] = PlayerToJson(player)
            };

            var attacker = GetHitInfoInitiatorPlayer(info);
            if (IsRealPlayer(attacker) && attacker != player)
            {
                data["attacker"] = PlayerToJson(attacker);
            }

            var pos = player.transform.position;
            data["position"] = new JObject
            {
                ["x"] = pos.x,
                ["y"] = pos.y,
                ["z"] = pos.z
            };

            SendGameEvent("player-death", data);
        }

        private void OnEntityDeath(BaseCombatEntity entity, object info)
        {
            // Real players die through OnPlayerDeath. NPCs (scientists etc.) are BasePlayers too
            // and are listed by listEntities, so a player killing one is an entity kill.
            if (entity == null || (entity is BasePlayer victim && !victim.IsNpc)) return;

            var attacker = GetHitInfoInitiatorPlayer(info);
            if (!IsRealPlayer(attacker)) return;

            var weapon = GetHitInfoWeaponShortName(info);
            var data = new JObject
            {
                ["player"] = PlayerToJson(attacker),
                ["entity"] = entity.ShortPrefabName ?? entity.GetType().Name,
                ["weapon"] = weapon
            };
            SendGameEvent("entity-killed", data);
        }

        private BasePlayer GetHitInfoInitiatorPlayer(object info)
        {
            if (info == null) return null;

            try
            {
                return GetMemberValue(info, "InitiatorPlayer") as BasePlayer;
            }
            catch (Exception ex)
            {
                LogDebug($"Unable to read HitInfo.InitiatorPlayer: {ex.Message}");
                return null;
            }
        }

        private string GetHitInfoWeaponShortName(object info)
        {
            if (info == null) return "";

            try
            {
                var weapon = GetMemberValue(info, "Weapon");
                var item = weapon?.GetType().GetMethod("GetItem", Type.EmptyTypes)?.Invoke(weapon, null);
                var itemInfo = GetMemberValue(item, "info");
                var shortName = GetMemberValue(itemInfo, "shortname") as string;
                if (!string.IsNullOrEmpty(shortName)) return shortName;

                var weaponPrefab = GetMemberValue(info, "WeaponPrefab");
                return GetMemberValue(weaponPrefab, "ShortPrefabName") as string ?? "";
            }
            catch (Exception ex)
            {
                LogDebug($"Unable to read HitInfo weapon: {ex.Message}");
                return "";
            }
        }

        private object GetMemberValue(object instance, string name)
        {
            if (instance == null) return null;

            var type = instance.GetType();
            var property = type.GetProperty(name);
            if (property != null) return property.GetValue(instance, null);

            var field = type.GetField(name);
            return field?.GetValue(instance);
        }

        private void OnServerMessage(string message, string username, string color, ulong userid)
        {
            if (string.IsNullOrEmpty(message)) return;
            if (message.StartsWith("%.") || message.StartsWith("[event]")) return;
            if (message.StartsWith("[Takaro") || message.StartsWith("[Carbon]")) return;
            if (message.StartsWith("SteamServer") || message.StartsWith("Saving complete")) return;

            var data = new JObject
            {
                ["msg"] = message
            };
            SendGameEvent("log", data);
        }

        // --- Logging Helpers ---

        // Carbon's file logger throws when the disk is full; a throw from a log call inside the
        // reconnect path ends the reconnect loop and leaves the server offline for good.
        private void LogInfo(string message) { try { Puts($"[Takaro] {message}"); } catch { } }
        private void LogWarning(string message) { try { PrintWarning($"[Takaro] {message}"); } catch { } }
        private void LogDebug(string message) { if (_debug) try { Puts($"[Takaro DEBUG] {message}"); } catch { } }

        // --- Game icon ---
        //
        // The 256 x 256 PNG Takaro shows for this game (Steam library art of app 252490).

        // takaro:icon-begin
        // Generated by scripts/embed-icon.sh from assets/icon-256.png; do not edit by hand.
        internal const string GameIconBase64 = @"
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
4ZxDBBGdTibvPLjPhMcnp0DgRh6l4iPVoQK1KlSTuiFGJvbsOLiT84tV204n9aSe/PL3n1ysVg/v3OXBz/0TcIYQkAjbrm/7/lIb
ZicPN3rnO1ukW8n5ENCggG3VdKgo7Te8WHE1UdUc01j4QlX1REeLA/vatw1IaGqqCmLBudm0PjxYzKdzJNxs2mcnxzGlum7OLi76
HM/Xqxdn58fnF+fLdd8nIqicm09nB/P5/TtH9w+P7h4ezufTSVW5USrYtJBMv1kq6Y+z+KMRUtvFL45fSNb7R3dS1i8ePa6qCpCe
n56amqi9ODmNKTlmu5qEwqOjw6wyb2Z35s1f//Sn9+ZzFb1YrupJ01S1SGYzDu6//f73j49PyPk+RjYoQoJNXVMZUiIy0NJwxGYK
ItL1bcyKxNNpNZ9MU9Zus/m7n/1VdUm3fSviH99sA9hfet/8s2+UeTYzx/T87Pz56ZljNrtS7t3jM8RBx7fwJt52ojJVZETn55Rl
ZGsrk0dFEa2pa0QqnqeIzprqR++8vWdqb0gYubcSW8HnKYzqdyq67rvlcrlabzKgpOTZueCqwJO6qb0PoSrxHhTCEzAd42Lcr4YP
auBfr7byDeHACAgv67K/7POqqvCr33/+X375y8ViXhGdb7bEzhMyUZdjyuLZBe+SSEppyF8ZYHkydd145k3bTqeTv3z/XVJDx72k
//wvvzg+u3h47yhU3lR+9PY7WbTddFYya4V9O/beMyGeLs8RyTuPMKgqMlNB83Y5t9tu227PLtbvPrj/9t0jUblRb7Nv+6TeqJRi
r+OoHGMAVCKMWdbrbUGMXGnEtZcvwDerP8MiiYzIQM65wOyYAzMze++8YyYqAQAiGoKaTurqYD7bYdfga/QY4f7djCu0iSiA1d4d
zGdHi5n3TkXefevBw3t3D+ezaVPvei9FRFXKzBkE9i7Pql8TlvsthtUAyWLKKuAcvapsCeDIAPjp2WlOuU+5CoEQRRUKlITQsduv
dpsaEXnvvfOuBBBEZDkjAHvXx3S0mC2msyfPT7LK0cEC1SZ1c3a+3Pa9ohIQAIiImm7b3hFn0Yt2E3OaVo2aDqq6ZiriGe8u7rHD
+XT64btva0p7buKtCWN9pY3aGz/xbzBIo18ICGruln6Xr9H3vMswIqCZAg7Q5IH2p8S7YojIxMWlVNUSfTlmBAOQG3bz6i/40r+W
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
s6sceYCnbTxouzbn4y46ZOi1jSiZ6Wa6hXg22doGwHa919MhRDGQwTqAiUGtVY1qnVhBPTxuGykMbKK1FQAAqKjht9GZsJ0E/8a9
DQY2FyESMaAxEhmaKZiYd64wBcwQUUEzWMpyVqHufb62NcBv2FvgJIcRAFUxsGRCpbo1hCQCJyLFRVXueljPbQBs1/vtMoANVbKh
ugUpTdNBEOXbbe8/dwC8lx+Dnb7e4aWjbc/046/z6tefnj7zt50nlL3Ixy+usUV5+Jp9Tru9V3/xh3+7AVB0XdGMEsiV8tc3DiQ7
vWOH/7ngw+CJFMIp/c711ufiiQDE/WawlNLL/BqYN/SlzbJkg6t2DyL2ylDXWDnnzf1tZ74EM2IeEGlWTBdFM6zdmm9YHZz+jm78
8c2sdF1hIyzvaF5Q3jMxU6+9fusBYCBkpFQ7mRDLTfe/nXh5nyiSb3yVYOs2Mg12PKf++PxR1/9DK1xZMARn1AG2lli1BJX3fjab
3flxhEWUxRBgtVjmE1cy293dBWKEC5i+5R3FGJumufqtrekm09nUOQ9nXC7VinghEeacV6tVceYWkdFoHEKlNjge3NmtW8B4BrBY
LEBPhKl3Zru9kS7cBoP9yvdAhKumSTEhbOiD3VYAGKk3PlL45/dGf/rD948k040i4KKfXbvfbQosrd1G1C44kWRDDcd6oSkT6OUh
dr39xXebn3orTgID4KpZfeYzn/m3/53fu1wuiS/Qqb3V88dMta7q/+zP/blf/eIXRqNRSmlnd+eP/fE/MZvtJpENY5nhE6iO6uqX
fvGX/pu/8Bd88Fdc30SUc96Zzf7on/iToa7NzHtf7C3MzLPrutYM9vZ2//bf/vn//qd/ugoVojVt8y/90A997l/74ePlnImR7jCJ
NzA0Y+L/5D/+ybe+9utVXcUY7z947Q//kT8yHo9E1BCZ+e7gEqo6m03/x5/+6f/785+fTaeDQuyt3gAERqDB2LM5LcfaDd7h+bwQ
+2vBAGhwocUiRry57fGKm7g39zEAEMN77Hd9TEhkoASqysyhrrrLvVtuMfs0sxCqolC53rgh1OS95wvUSkWFQwi+uo70bTnwp7Mp
EEvOxRoaEdu2RaZqPELEmBMyD7b0qADkXDWq65zumntVLmQPzOTWn4aYq9Eo1KMi53qnb0BVnfM9PgNPm9HeVg2QAKSXHDGDm5Yb
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
TkSuQmCC
";
        // takaro:icon-end
    }
}
