using System;
using System.Collections.Generic;
using System.IO;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro.Services;

namespace Takaro.WebSocket
{
    public static class ProtocolErrorCodes
    {
        public const string Unsupported = "unsupported";
        public const string InvalidArgs = "invalid_args";
        public const string NotFound = "not_found";
        public const string GameError = "game_error";
        public const string Internal = "internal";
        public const string NoSharedProtocolVersion = "no_shared_protocol_version";
    }

    public static class GameEventTypes
    {
        public const string PlayerConnected = "player-connected";
        public const string PlayerDisconnected = "player-disconnected";
        public const string ChatMessage = "chat-message";
        public const string PlayerDeath = "player-death";
        public const string EntityKilled = "entity-killed";
        public const string Log = "log";

        public static readonly string[] All =
        {
            PlayerConnected,
            PlayerDisconnected,
            ChatMessage,
            PlayerDeath,
            EntityKilled,
            Log,
        };
    }

    public class IdentifyReply
    {
        public bool Accepted;
        public string GameServerId;

        /// <summary>0 when Takaro's reply names no protocol version.</summary>
        public int ProtocolVersion;

        /// <summary>The v1 rejection code; null for a v0 rejection, which has none.</summary>
        public string ErrorCode;

        /// <summary>Bounded, single-line reason for a rejection.</summary>
        public string Reason;
    }

    /// <summary>
    /// The identify handshake of Generic Game Protocol v1: what the mod offers
    /// (versions, Capability Manifest, game identifier, Connector Migration
    /// support), how Takaro's answer is read, and how game events are numbered
    /// into a stream. Pure data and parsing; no game or socket types.
    /// </summary>
    public static class ProtocolHandshake
    {
        public const string GameIdentifier = "7d2d";

        /// <summary>
        /// Declaring migration.native promises the parity contract of Takaro's
        /// built-in integration for this game (gettakaro/connectors#419,
        /// gettakaro/takaro#4349).
        /// </summary>
        public const string MigrationSource = "7d2d";

        public const string MigrationCapability = "migration.native";

        public static readonly int[] OfferedProtocolVersions = { 0, 1 };

        // testReachability is always allowed and the map actions are not declared:
        // the mod cannot yet answer v1's tiled map requests, so Takaro must not ask.
        public static readonly IDictionary<string, string> ActionCapabilities = new Dictionary<
            string,
            string
        >
        {
            { "getPlayers", "players.list" },
            { "getPlayer", "players.get" },
            { "getPlayerLocation", "players.location" },
            { "getPlayerInventory", "players.inventory" },
            { "teleportPlayer", "players.teleport" },
            { "listItems", "items.list" },
            { "giveItem", "items.give" },
            { "listEntities", "entities.list" },
            { "listLocations", "locations.list" },
            { "sendMessage", "chat.send" },
            { "executeConsoleCommand", "console.execute" },
            { "kickPlayer", "moderation.kick" },
            { "banPlayer", "moderation.ban" },
            { "unbanPlayer", "moderation.unban" },
            { "listBans", "moderation.listBans" },
            { "shutdown", "server.shutdown" },
        };

        public static List<Dictionary<string, object>> BuildCapabilityManifest(
            IEnumerable<string> handledActions
        )
        {
            var manifest = new List<Dictionary<string, object>>();
            var declared = new HashSet<string>();
            foreach (string action in handledActions)
            {
                if (
                    ActionCapabilities.TryGetValue(action, out string capability)
                    && declared.Add(capability)
                )
                    manifest.Add(new Dictionary<string, object> { { "name", capability } });
            }

            foreach (string eventType in GameEventTypes.All)
                manifest.Add(new Dictionary<string, object> { { "name", "events." + eventType } });

            manifest.Add(
                new Dictionary<string, object>
                {
                    { "name", MigrationCapability },
                    {
                        "parameters",
                        new Dictionary<string, object> { { "from", MigrationSource } }
                    },
                }
            );
            return manifest;
        }

        public static Dictionary<string, object> BuildIdentifyPayload(
            string registrationToken,
            string identityToken
        )
        {
            return new Dictionary<string, object>
            {
                { "registrationToken", registrationToken },
                { "identityToken", identityToken },
                { "protocolVersions", OfferedProtocolVersions },
                { "capabilities", BuildCapabilityManifest(RequestRouter.SupportedActions) },
                { "game", GameIdentifier },
            };
        }

        public static IdentifyReply ParseIdentifyResponse(string json)
        {
            if (
                ProtocolDiagnostics.TryGetIdentifyRejection(
                    json,
                    out string code,
                    out string reason
                )
            )
                return new IdentifyReply { ErrorCode = code, Reason = reason };

            JObject payload = JObject.Parse(json)["payload"] as JObject;
            if (payload == null)
                return new IdentifyReply { Reason = "identifyResponse carries no payload" };

            JToken versionToken = payload["protocolVersion"];
            int version = 0;
            if (versionToken != null && versionToken.Type != JTokenType.Null)
            {
                if (
                    versionToken.Type != JTokenType.Integer
                    || Array.IndexOf(OfferedProtocolVersions, (int)versionToken) < 0
                )
                {
                    return new IdentifyReply
                    {
                        ErrorCode = ProtocolErrorCodes.NoSharedProtocolVersion,
                        Reason =
                            $"Takaro selected protocol version {versionToken}, which this mod does not speak",
                    };
                }
                version = (int)versionToken;
            }

            return new IdentifyReply
            {
                Accepted = true,
                GameServerId = (string)payload["gameServerId"],
                ProtocolVersion = version,
            };
        }

        /// <summary>
        /// Adds the stream position Takaro dedupes on. Numbers and dates are
        /// kept as written so the event data is not altered by the round trip.
        /// </summary>
        public static string WithStreamPosition(string eventJson, string streamId, long seq)
        {
            JObject frame;
            using (var reader = new JsonTextReader(new StringReader(eventJson)))
            {
                reader.DateParseHandling = DateParseHandling.None;
                frame = JObject.Load(reader);
            }

            var payload = (JObject)frame["payload"];
            payload["streamId"] = streamId;
            payload["seq"] = seq;
            return frame.ToString(Formatting.None);
        }

        public static bool TryParseEventAck(string json, out string streamId, out long upTo)
        {
            streamId = null;
            upTo = 0;
            JObject payload = ParseFrame(json, "eventAck");
            if (payload == null)
                return false;

            streamId = (string)payload["streamId"];
            JToken upToToken = payload["upTo"];
            if (
                string.IsNullOrEmpty(streamId)
                || upToToken == null
                || upToToken.Type != JTokenType.Integer
            )
                return false;

            upTo = (long)upToToken;
            return true;
        }

        public static bool TryParseEventError(
            string json,
            out string streamId,
            out long? seq,
            out string code,
            out string message
        )
        {
            streamId = null;
            seq = null;
            code = null;
            message = null;
            JObject frame = ParseFrameObject(json, "eventError");
            if (frame == null)
                return false;

            JObject payload = frame["payload"] as JObject;
            JObject error = frame["error"] as JObject;
            streamId = (string)payload?["streamId"];
            JToken seqToken = payload?["seq"];
            if (seqToken != null && seqToken.Type == JTokenType.Integer)
                seq = (long)seqToken;
            code = (string)error?["code"];
            message = error == null ? null : ProtocolDiagnostics.ExtractErrorMessage(error);
            return true;
        }

        private static JObject ParseFrame(string json, string expectedType)
        {
            return ParseFrameObject(json, expectedType)?["payload"] as JObject;
        }

        private static JObject ParseFrameObject(string json, string expectedType)
        {
            if (string.IsNullOrEmpty(json))
                return null;

            JObject frame;
            try
            {
                frame = JObject.Parse(json);
            }
            catch (JsonException)
            {
                return null;
            }

            return (string)frame["type"] == expectedType ? frame : null;
        }
    }
}
