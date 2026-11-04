using System.Collections.Generic;
using Newtonsoft.Json.Linq;

namespace TakaroEmpyrion.Protocol
{
    /// <summary>The Capability Manifest a protocol v1 identify carries. Anything not declared is unsupported.</summary>
    public static class Capabilities
    {
        public static readonly int[] OfferedVersions = { 0, 1 };

        /// <summary>
        /// Takaro's action -> Capability table. testReachability is always allowed; getMapTile is covered by the
        /// `map` Capability, declared only when a planet has terrain; getMapInfo is protocol v0 only.
        /// </summary>
        public static readonly IDictionary<string, string> ForAction = new Dictionary<string, string>
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

        public static readonly string[] EventTypes = { "player-connected", "player-disconnected", "chat-message", "player-death", "entity-killed", "log" };

        public static JArray Build(IEnumerable<string> actions, JObject mapCapability)
        {
            var manifest = new JArray();
            var seen = new HashSet<string>();
            foreach (string action in actions)
                if (ForAction.TryGetValue(action, out string cap) && seen.Add(cap)) manifest.Add(new JObject { ["name"] = cap });
            foreach (string type in EventTypes) manifest.Add(new JObject { ["name"] = "events." + type });
            if (mapCapability != null) manifest.Add(mapCapability);
            return manifest;
        }
    }

    /// <summary>The error codes of a protocol v1 response.</summary>
    public static class ErrorCodes
    {
        public const string Unsupported = "unsupported", InvalidArgs = "invalid_args", NotFound = "not_found",
            GameError = "game_error", Internal = "internal";
    }
}
