using System.Collections.Generic;

namespace TakaroEmpyrion.Core
{
    /// <summary>
    /// One record per Takaro action and event. capabilities.json is asserted equal to this table by the
    /// drift test, so docs cannot drift from behaviour. Only a live run may promote to live-supported.
    /// </summary>
    public static class Coverage
    {
        public sealed class Entry
        {
            public string Name, Kind, Status, ResponseShape, Reason;
            public Entry(string name, string kind, string status, string shape, string reason)
            { Name = name; Kind = kind; Status = status; ResponseShape = shape; Reason = reason; }
        }

        public const string Live = "live-supported", Fallback = "schema-fallback", Unsupported = "unsupported";

        /// <summary>Entries whose status has been proven end to end on a real server with a real client.
        /// Empty until the first live campaign; the README only shows ✅ for these.</summary>
        public static readonly HashSet<string> LiveVerified = new HashSet<string>();

        public static readonly List<Entry> All = new List<Entry>
        {
            new Entry("testReachability", "action", Live, "{connectable,reason}", "Request_Dedi_Stats round-trip through the mod API"),
            new Entry("getPlayers", "action", Live, "IGamePlayer[]", "Request_Player_List + Request_Player_Info; gameId = SteamID64"),
            new Entry("getPlayer", "action", Live, "IGamePlayer|null", "online via Request_Player_Info, offline from players.json"),
            new Entry("getPlayerLocation", "action", Live, "{x,y,z,dimension}|null", "PlayerInfo.pos; dimension = playfield as map Dimension id"),
            new Entry("getPlayerInventory", "action", Live, "IItemDTO[]", "Request_Player_GetInventory toolbelt + bag"),
            new Entry("getPlayerProgression", "action", Live, "ProgressionTrack[]|null", "PlayerInfo.exp; level from the game's own rule; last reading kept for offline players"),
            new Entry("giveItem", "action", Live, "null", "Request_Player_AddItem by config name, id or display name"),
            new Entry("listItems", "action", Live, "IItemDTO[]", "GetBlockAndItemMapping + Localization.csv display names"),
            new Entry("listEntities", "action", Live, "IEntityDTO[]", "EClassConfig.ecf creatures with a faction, localized names"),
            new Entry("listLocations", "action", Fallback, "[]", "no named world locations a server mod can list; not declared on protocol v1"),
            new Entry("executeConsoleCommand", "action", Live, "{success,rawResult,errorMessage}", "loopback telnet console (real output), allowlisted"),
            new Entry("sendMessage", "action", Live, "null", "IApplication.SendChatMessage: global or SinglePlayer, SenderNameOverride"),
            new Entry("teleportPlayer", "action", Live, "null", "Request_Entity_Teleport; Request_Player_ChangePlayerfield for another dimension"),
            new Entry("kickPlayer", "action", Live, "null", "console kick <steamid> 'reason'"),
            new Entry("banPlayer", "action", Live, "null", "console ban <steamid> <h|d>; reason kept in bans.json"),
            new Entry("unbanPlayer", "action", Live, "null", "console unban <steamid>, works offline"),
            new Entry("listBans", "action", Live, "IBanDTO[]", "Request_GetBannedPlayers (SteamID + end time)"),
            new Entry("shutdown", "action", Live, "null", "console saveandexit 0, gated by ENABLE_DESTRUCTIVE_COMMANDS"),
            new Entry("getMapInfo", "action", Unsupported, "-", "protocol v0 only; the v1 map capability replaces it"),
            new Entry("getMapTile", "action", Live, "{mimeType,data}", "protocol v1: one Dimension per generated planet, tiles rendered from terrain.raw + biomes.dat"),
            new Entry("getGameIcon", "action", Live, "{revision,data}", "protocol v1: the shipped icon, declared in game.profile"),
            new Entry("player-connected", "event", Live, "EventPlayerConnected", "Event_Player_Connected"),
            new Entry("player-disconnected", "event", Live, "EventPlayerDisconnected", "Event_Player_Disconnected"),
            new Entry("chat-message", "event", Live, "EventChatMessage", "IApplication.ChatMessageSent (Event_ChatMessage fallback)"),
            new Entry("player-death", "event", Live, "EventPlayerDeath", "Event_Statistics PlayerDied"),
            new Entry("entity-killed", "event", Live, "EventEntityKilled", "Event_GameEvent KilledEntity"),
            new Entry("log", "event", Live, "EventLogLine", "playfield load/unload, budgeted"),
        };
    }
}
