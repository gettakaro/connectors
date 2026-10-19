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
    [Info("TakaroConnector", "Takaro", "0.3.0")] // x-release-please-version
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

        internal sealed class ConnectorSettings
        {
            public string WsUrl = DefaultWsUrl;
            public string RegistrationToken = "";
            public string IdentityToken = "";
            public bool Debug;
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

            public bool HasAnyKey
            {
                get { return RegistrationToken != null || IdentityToken != null || WsUrl != null || Debug != null; }
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

        internal static JObject BuildIdentifyPayload(string identityToken, string registrationToken, JArray manifest)
        {
            return new JObject
            {
                ["identityToken"] = identityToken ?? "",
                ["registrationToken"] = registrationToken ?? "",
                ["protocolVersions"] = new JArray(OfferedProtocolVersions),
                ["capabilities"] = manifest,
            };
        }

        internal sealed class IdentifyReply
        {
            public bool Accepted;
            public string GameServerId;
            public int ProtocolVersion;
            // The v1 rejection code; null for a v0 rejection, which has none.
            public string ErrorCode;
            public string Reason;
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

                    if (code == 1008 || code == 4001 || code == 4003)
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
            return BuildManifest(_mapCapability);
        }

        private void SendIdentify(ClientWebSocket ws, int generation, ConnectorSettings settings)
        {
            // At most once per connection: the socket opening and a server greeting both
            // lead here, and a second identify would look like a second session.
            if (Interlocked.Exchange(ref _identifySentGeneration, generation) == generation) return;

            var msg = new JObject
            {
                ["type"] = "identify",
                ["payload"] = BuildIdentifyPayload(settings.IdentityToken, settings.RegistrationToken, CurrentManifest())
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
                case "getPlayerLocation": return HandleGetPlayerLocation(args);
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

        private JToken HandleGetPlayerLocation(JObject args)
        {
            var gameId = GameIdFrom(args);
            var player = FindPlayerByGameId(gameId);

            if (player != null)
            {
                var pos = player.transform.position;
                _lastPosition[gameId] = pos;
                return new JObject
                {
                    ["x"] = pos.x,
                    ["y"] = pos.y,
                    ["z"] = pos.z
                };
            }

            if (_lastPosition.TryGetValue(gameId, out var cached))
            {
                return new JObject
                {
                    ["x"] = cached.x,
                    ["y"] = cached.y,
                    ["z"] = cached.z
                };
            }

            return new JObject { ["x"] = 0, ["y"] = 0, ["z"] = 0 };
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

                if (prefab.GetComponent<BaseNpc>() == null && prefab.GetComponent<NPCPlayer>() == null) continue;

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
            var message = args.Value<string>("message") ?? "";
            string recipientGameId = null;

            var opts = args["opts"] as JObject;
            var recipient = opts?["recipient"] as JObject;
            recipientGameId = recipient?.Value<string>("gameId");

            if (!string.IsNullOrEmpty(recipientGameId))
            {
                var player = FindPlayerByGameId(recipientGameId);
                player?.ChatMessage(message);
            }
            else
            {
                // Same reasoning as the console audit line above: a broadcast leaves no
                // trace in Rust's own console, so nothing on the server would ever show
                // that Takaro said something to everybody.
                LogInfo($"broadcast: {message}");
                ConsoleNetwork.BroadcastToAllClients("chat.add", 2, 0, message);
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
    }
}
